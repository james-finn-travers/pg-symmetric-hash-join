/*-------------------------------------------------------------------------
 *
 * nodeHashjoin.c
 *	  Routines to handle hash join nodes
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/executor/nodeHashjoin.c,v 1.75.2.4 2007/02/02 00:07:44 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/hashjoin.h"
#include "executor/nodeHash.h"
#include "executor/nodeHashjoin.h"
#include "optimizer/clauses.h"
#include "utils/memutils.h"


static TupleTableSlot *ExecHashJoinGetSavedTuple(HashJoinState *hjstate,
						  BufFile *file,
						  uint32 *hashvalue,
						  TupleTableSlot *tupleSlot);
static int	ExecHashJoinNewBatch(HashJoinState *hjstate);


/* ----------------------------------------------------------------
 *		ExecHashJoin
 *
 *		Symmetric hash join:
 *		Builds and maintains two hash tables, for inner and outer input
 *		Join alternates between reading in and out inputs unless one is finished
 *		Each call makes at most a single joined result tuple
 *		
 * ----------------------------------------------------------------
 */
TupleTableSlot *				/* return: a tuple or NULL */
ExecHashJoin(HashJoinState *node)
{
	HashState *hashNode;
	PlanState *outerNode;
	PlanState *innerChild;
	PlanState *outerChild;

	List *joinqual;
	List *otherqual;
	ExprContext *econtext;
	ExprDoneCond isDone;

	HashJoinTable innerTable;
	HashJoinTable outerTable;

	HeapTuple curtuple;
	TupleTableSlot *innTup;
	TupleTableSlot *drvSlot;

	int bucketno;
	int batchno;
	uint32 hval_insert;

	joinqual = node->js.joinqual;
	otherqual = node->js.ps.qual;
	hashNode = (HashState *) innerPlanState(node);
	outerNode = outerPlanState(node);
	econtext = node->js.ps.ps_ExprContext;

	/* Get the active inner and outer hash tables */
	innerTable = node->hj_HashTable;
	outerTable = node->hj_OuterHashTable;

	/* 
	 * Retrieve the scanner child nodes:
	 *   innerChild: child scan of the inner Hash node
	 *   outerChild: child scan of the outer Hash node
	 */
	innerChild = outerPlanState((PlanState *) hashNode);
	outerChild = outerPlanState(outerNode);

	/* Check if we are in the middle of returning a multi-result projection */
	if (node->js.ps.ps_TupFromTlist) {
		TupleTableSlot *result;
		result = ExecProject(node->js.ps.ps_ProjInfo, &isDone);
		if (isDone == ExprMultipleResult) {
			return result;
		}
		node->js.ps.ps_TupFromTlist = false;
	}

	/* Symmetrical Hash Join currently supports INNER joins only */
	if (node->js.jointype != JOIN_INNER) {
		elog(ERROR, "Symmetric Hash Join only supports INNER joins");
	}

	ResetExprContext(econtext);

	/* Lazily instantiate the hash tables on the first call */
	if (innerTable == NULL) {
		HashState *innerHashState = (HashState *) innerPlanState(node);
		HashState *outerHashState = (PlanState *) outerPlanState(node);

		innerTable = ExecHashTableCreate((Hash *) innerHashState->ps.plan, node->hj_HashOperators);
		node->hj_HashTable = innerTable;
		
		outerTable = ExecHashTableCreate((Hash *) outerHashState->ps.plan, node->hj_HashOperators);
		node->hj_OuterHashTable = outerTable;

		node->hj_InnerFinished = false;
		node->hj_OuterFinished = false;
		node->hj_ReadInnerNext = true;

		node->hj_InnerMatchCount = 0;
		node->hj_OuterMatchCount = 0;
	}

	/*
	 * Main demand-pull state loop. Each iteration aims to produce one output
	 * tuple or return NULL when execution is complete.
	 */
	while (1) {
		/* Continue scanning matches for the current driver tuple if probing is active */
		if (!node->hj_NeedNewOuter) {
			while (1) {
				curtuple = ExecScanHashBucket(node, econtext);
				if (curtuple == NULL) {
					break; /* Bucket exhausted */
				}

				/* 
				 * Store matched tuple in the appropriate slot depending on target table:
				 *   Probing inner table (0): scanned tuple is inner, driver tuple is outer
				 *   Probing outer table (1): scanned tuple is outer, driver tuple is inner
				 */
				if (node->hj_CurrentTable == 0) {
					innTup = ExecStoreTuple(curtuple, node->hj_HashTupleSlot, InvalidBuffer, false);
					econtext->ecxt_innertuple = innTup;
					econtext->ecxt_outertuple = node->hj_OuterTupleSlot;
				} else {
					innTup = ExecStoreTuple(curtuple, node->hj_OuterHashTupleSlot, InvalidBuffer, false);
					econtext->ecxt_outertuple = innTup;
					econtext->ecxt_innertuple = node->hj_OuterTupleSlot;
				}

				ResetExprContext(econtext);

				/* Evaluate join conditions and projections */
				if (joinqual == NIL || ExecQual(joinqual, econtext, false)) {
					if (node->hj_CurrentTable == 0) {
						node->hj_InnerMatchCount++;
					} else {
						node->hj_OuterMatchCount++;
					}

					if (otherqual == NIL || ExecQual(otherqual, econtext, false)) {
						TupleTableSlot *result;
						result = ExecProject(node->js.ps.ps_ProjInfo, &isDone);
						if (isDone != ExprEndResult) {
							node->js.ps.ps_TupFromTlist = (isDone == ExprMultipleResult);
							node->hj_MatchedOuter = true;
							return result;
						}
					}

					if (node->js.jointype == JOIN_IN) {
						node->hj_NeedNewOuter = true;
						break;
					}
				}
			}

			node->hj_NeedNewOuter = true;
		}

		/* Both inputs exhausted: join is complete */
		if (node->hj_InnerFinished && node->hj_OuterFinished) {
			elog(LOG, "Symmetric hash join: %ld matches found using the inner hash table, %ld matches found using the outer hash table", node->hj_InnerMatchCount, node->hj_OuterMatchCount);
			return NULL;
		}

		/*
		 * Alternating driving logic:
		 * Read from the inner child next if:
		 *   1. Inner is not finished, and
		 *   2. Either inner's turn is next or the outer input is exhausted.
		 */
		if (!node->hj_InnerFinished && (node->hj_ReadInnerNext || node->hj_OuterFinished)) {
			drvSlot = ExecProcNode(innerChild);

			if (TupIsNull(drvSlot)) {
				node->hj_InnerFinished = true;
				node->hj_ReadInnerNext = false;
				continue;
			}
			
			econtext->ecxt_innertuple = drvSlot;
			ResetExprContext(econtext);

			/* Insert driver tuple into the inner hash table */
			hval_insert = ExecHashGetHashValue(innerTable, econtext, node->hj_InnerHashKeys);
			ExecHashTableInsert(innerTable, ExecFetchSlotTuple(drvSlot), hval_insert);

			/* Save driver tuple for probe matching */
			node->js.ps.ps_OuterTupleSlot = drvSlot;
			node->hj_OuterTupleSlot = drvSlot;

			/* Switch scanning target to the opposite table (probe outer table) */
			node->hj_CurrentTable = 1;

			/* Use inner hash keys to probe the outer table */
			node->hj_OuterHashValue = ExecHashGetHashValue(outerTable, econtext, node->hj_InnerHashKeys);
			ExecHashGetBucketAndBatch(outerTable, node->hj_OuterHashValue, &bucketno, &batchno);

			node->hj_OuterBucketNo = bucketno;
			node->hj_OuterCurTuple = NULL;

			node->hj_NeedNewOuter = false;
			node->hj_MatchedOuter = false;

			/* Alternate driver direction for the next iteration */
			if (!node->hj_OuterFinished) {
				node->hj_ReadInnerNext = false;
			} else {
				node->hj_ReadInnerNext = true;
			}
			continue;
		} else { 
			/* Read from the outer child input */
			drvSlot = ExecProcNode(outerChild);
			if (TupIsNull(drvSlot)) {
				node->hj_OuterFinished = true;
				node->hj_ReadInnerNext = true;
				continue;
			}
			econtext->ecxt_outertuple = drvSlot;
			node->js.ps.ps_OuterTupleSlot = drvSlot;
			node->hj_OuterTupleSlot = drvSlot;

			/* Insert driver tuple into the outer hash table */
			hval_insert = ExecHashGetHashValue(outerTable, econtext, node->hj_OuterHashKeys);
			ExecHashTableInsert(outerTable, ExecFetchSlotTuple(drvSlot), hval_insert);

			/* Switch scanning target to the opposite table (probe inner table) */
			node->hj_CurrentTable = 0;

			/* Use outer hash keys to probe the inner table */
			node->hj_CurHashValue = ExecHashGetHashValue(innerTable, econtext, node->hj_OuterHashKeys);
			ExecHashGetBucketAndBatch(innerTable, node->hj_CurHashValue, &bucketno, &batchno);

			node->hj_InnerBucketNo = bucketno;
			node->hj_CurTuple = NULL;

			node->hj_NeedNewOuter = false;
			node->hj_MatchedOuter = false;

			/* Alternate driver direction for the next iteration */
			if (!node->hj_InnerFinished) {
				node->hj_ReadInnerNext = true;
			}
			continue;
		}
	}
}


/* ----------------------------------------------------------------
 *		ExecInitHashJoin
 *
 *		Init routine for HashJoin node.
 * ----------------------------------------------------------------
 */
HashJoinState *
ExecInitHashJoin(HashJoin *node, EState *estate)
{
	HashJoinState *hjstate;
	Plan	   *outerNode;
	Hash	   *hashNode;
	List	   *lclauses;
	List	   *rclauses;
	List	   *hoperators;
	ListCell   *l;

	/*
	 * create state structure
	 */
	hjstate = makeNode(HashJoinState);
	hjstate->js.ps.plan = (Plan *) node;
	hjstate->js.ps.state = estate;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &hjstate->js.ps);

	/*
	 * initialize child expressions
	 */
	hjstate->js.ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->join.plan.targetlist,
					 (PlanState *) hjstate);
	hjstate->js.ps.qual = (List *)
		ExecInitExpr((Expr *) node->join.plan.qual,
					 (PlanState *) hjstate);
	hjstate->js.jointype = node->join.jointype;
	hjstate->js.joinqual = (List *)
		ExecInitExpr((Expr *) node->join.joinqual,
					 (PlanState *) hjstate);
	hjstate->hashclauses = (List *)
		ExecInitExpr((Expr *) node->hashclauses,
					 (PlanState *) hjstate);

	/*
	 * initialize child nodes
	 */
	outerNode = outerPlan(node);
	hashNode = (Hash *) innerPlan(node);

	outerPlanState(hjstate) = ExecInitNode(outerNode, estate);
	innerPlanState(hjstate) = ExecInitNode((Plan *) hashNode, estate);

#define HASHJOIN_NSLOTS 3

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &hjstate->js.ps);
	hjstate->hj_OuterTupleSlot = ExecInitExtraTupleSlot(estate);

	switch (node->join.jointype)
	{
		case JOIN_INNER:
		case JOIN_IN:
			break;
		case JOIN_LEFT:
			hjstate->hj_NullInnerTupleSlot =
				ExecInitNullTupleSlot(estate,
								 ExecGetResultType(innerPlanState(hjstate)));
			break;
		default:
			elog(ERROR, "unrecognized join type: %d",
				 (int) node->join.jointype);
	}

	/*
	 * now for some voodoo.  our temporary tuple slot is actually the result
	 * tuple slot of the Hash node (which is our inner plan).  we do this
	 * because Hash nodes don't return tuples via ExecProcNode() -- instead
	 * the hash join node uses ExecScanHashBucket() to get at the contents of
	 * the hash table.	-cim 6/9/91
	 */
	{
		HashState  *innerHashState = (HashState *) innerPlanState(hjstate);
		HashState  *outerHashState = (HashState *) outerPlanState(hjstate);

		hjstate->hj_HashTupleSlot = innerHashState->ps.ps_ResultTupleSlot;
		hjstate->hj_OuterHashTupleSlot = outerHashState->ps.ps_ResultTupleSlot;
	}

	/*
	 * initialize tuple type and projection info
	 */
	ExecAssignResultTypeFromTL(&hjstate->js.ps);
	ExecAssignProjectionInfo(&hjstate->js.ps);

	ExecSetSlotDescriptor(hjstate->hj_OuterTupleSlot,
						  ExecGetResultType(outerPlanState(hjstate)),
						  false);

	/* Symmetric hash join runtime state */
	hjstate->hj_HashTable = NULL;
	hjstate->hj_OuterHashTable = NULL;
	hjstate->hj_FirstOuterTupleSlot = NULL;

	hjstate->hj_CurHashValue = 0;
	hjstate->hj_CurTuple = NULL;
	hjstate->hj_InnerBucketNo = 0;

	hjstate->hj_OuterHashValue = 0;
	hjstate->hj_OuterCurTuple = NULL;
	hjstate->hj_OuterBucketNo = 0;

	hjstate->hj_CurrentTable = 0;
	hjstate->hj_ReadInnerNext = true;
	hjstate->hj_InnerFinished = false;
	hjstate->hj_OuterFinished = false;

	hjstate->hj_InnerMatchCount = 0;
	hjstate->hj_OuterMatchCount = 0;

	/*
	 * Deconstruct the hash clauses into outer and inner argument values, so
	 * that we can evaluate those subexpressions separately.  Also make a list
	 * of the hash operator OIDs, in preparation for looking up the hash
	 * functions to use.
	 */
	lclauses = NIL;
	rclauses = NIL;
	hoperators = NIL;
	foreach(l, hjstate->hashclauses)
	{
		FuncExprState *fstate = (FuncExprState *) lfirst(l);
		OpExpr	   *hclause;

		Assert(IsA(fstate, FuncExprState));
		hclause = (OpExpr *) fstate->xprstate.expr;
		Assert(IsA(hclause, OpExpr));
		lclauses = lappend(lclauses, linitial(fstate->args));
		rclauses = lappend(rclauses, lsecond(fstate->args));
		hoperators = lappend_oid(hoperators, hclause->opno);
	}
	hjstate->hj_OuterHashKeys = lclauses;
	hjstate->hj_InnerHashKeys = rclauses;
	hjstate->hj_HashOperators = hoperators;
	/* child Hash node needs to evaluate inner hash keys, too */
	((HashState *) innerPlanState(hjstate))->hashkeys = rclauses;

	/* original PostgreSQL control flags */
	hjstate->js.ps.ps_OuterTupleSlot = NULL;
	hjstate->js.ps.ps_TupFromTlist = false;
	hjstate->hj_NeedNewOuter = true;
	hjstate->hj_MatchedOuter = false;
	hjstate->hj_OuterNotEmpty = false;

	return hjstate;
}

int
ExecCountSlotsHashJoin(HashJoin *node)
{
	return ExecCountSlotsNode(outerPlan(node)) +
		ExecCountSlotsNode(innerPlan(node)) +
		HASHJOIN_NSLOTS;
}

/* ----------------------------------------------------------------
 *		ExecEndHashJoin
 *
 *		clean up routine for HashJoin node
 * ----------------------------------------------------------------
 */
void
ExecEndHashJoin(HashJoinState *node)
{
	/*
	 * Free hash table
	 */
	if (node->hj_HashTable)
	{
		ExecHashTableDestroy(node->hj_HashTable);
		node->hj_HashTable = NULL;
	}

	if (node->hj_OuterHashTable)
	{
		ExecHashTableDestroy(node->hj_OuterHashTable);
		node->hj_OuterHashTable = NULL;
	}

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->js.ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->js.ps.ps_ResultTupleSlot);
	ExecClearTuple(node->hj_OuterTupleSlot);
	ExecClearTuple(node->hj_HashTupleSlot);

	/*
	 * clean up subtrees
	 */
	ExecEndNode(outerPlanState(node));
	ExecEndNode(innerPlanState(node));
}

/*
 * ExecHashJoinNewBatch
 *		switch to a new hashjoin batch
 *
 * Returns the number of the new batch (1..nbatch-1), or nbatch if no more.
 * We will never return a batch number that has an empty outer batch file.
 */
static int
ExecHashJoinNewBatch(HashJoinState *hjstate)
{
	HashJoinTable hashtable = hjstate->hj_HashTable;
	int			nbatch;
	int			curbatch;
	BufFile    *innerFile;
	TupleTableSlot *slot;
	uint32		hashvalue;

start_over:
	nbatch = hashtable->nbatch;
	curbatch = hashtable->curbatch;

	if (curbatch > 0)
	{
		/*
		 * We no longer need the previous outer batch file; close it right
		 * away to free disk space.
		 */
		if (hashtable->outerBatchFile[curbatch])
			BufFileClose(hashtable->outerBatchFile[curbatch]);
		hashtable->outerBatchFile[curbatch] = NULL;
	}

	/*
	 * We can always skip over any batches that are completely empty on both
	 * sides.  We can sometimes skip over batches that are empty on only one
	 * side, but there are exceptions:
	 *
	 * 1. In a LEFT JOIN, we have to process outer batches even if the inner
	 * batch is empty.
	 *
	 * 2. If we have increased nbatch since the initial estimate, we have to
	 * scan inner batches since they might contain tuples that need to be
	 * reassigned to later inner batches.
	 *
	 * 3. Similarly, if we have increased nbatch since starting the outer
	 * scan, we have to rescan outer batches in case they contain tuples that
	 * need to be reassigned.
	 */
	curbatch++;
	while (curbatch < nbatch &&
		   (hashtable->outerBatchFile[curbatch] == NULL ||
			hashtable->innerBatchFile[curbatch] == NULL))
	{
		if (hashtable->outerBatchFile[curbatch] &&
			hjstate->js.jointype == JOIN_LEFT)
			break;				/* must process due to rule 1 */
		if (hashtable->innerBatchFile[curbatch] &&
			nbatch != hashtable->nbatch_original)
			break;				/* must process due to rule 2 */
		if (hashtable->outerBatchFile[curbatch] &&
			nbatch != hashtable->nbatch_outstart)
			break;				/* must process due to rule 3 */
		/* We can ignore this batch. */
		/* Release associated temp files right away. */
		if (hashtable->innerBatchFile[curbatch])
			BufFileClose(hashtable->innerBatchFile[curbatch]);
		hashtable->innerBatchFile[curbatch] = NULL;
		if (hashtable->outerBatchFile[curbatch])
			BufFileClose(hashtable->outerBatchFile[curbatch]);
		hashtable->outerBatchFile[curbatch] = NULL;
		curbatch++;
	}

	if (curbatch >= nbatch)
		return curbatch;		/* no more batches */

	hashtable->curbatch = curbatch;

	/*
	 * Reload the hash table with the new inner batch (which could be empty)
	 */
	ExecHashTableReset(hashtable);

	innerFile = hashtable->innerBatchFile[curbatch];

	if (innerFile != NULL)
	{
		if (BufFileSeek(innerFile, 0, 0L, SEEK_SET))
			ereport(ERROR,
					(errcode_for_file_access(),
				   errmsg("could not rewind hash-join temporary file: %m")));

		while ((slot = ExecHashJoinGetSavedTuple(hjstate,
												 innerFile,
												 &hashvalue,
												 hjstate->hj_HashTupleSlot)))
		{
			/*
			 * NOTE: some tuples may be sent to future batches.  Also, it is
			 * possible for hashtable->nbatch to be increased here!
			 */
			ExecHashTableInsert(hashtable,
								ExecFetchSlotTuple(slot),
								hashvalue);
		}

		/*
		 * after we build the hash table, the inner batch file is no longer
		 * needed
		 */
		BufFileClose(innerFile);
		hashtable->innerBatchFile[curbatch] = NULL;
	}

	/*
	 * If there's no outer batch file, advance to next batch.
	 */
	if (hashtable->outerBatchFile[curbatch] == NULL)
		goto start_over;

	/*
	 * Rewind outer batch file, so that we can start reading it.
	 */
	if (BufFileSeek(hashtable->outerBatchFile[curbatch], 0, 0L, SEEK_SET))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rewind hash-join temporary file: %m")));

	return curbatch;
}

/*
 * ExecHashJoinSaveTuple
 *		save a tuple to a batch file.
 *
 * The data recorded in the file for each tuple is its hash value,
 * then an image of its HeapTupleData (with meaningless t_data pointer)
 * followed by the HeapTupleHeader and tuple data.
 *
 * Note: it is important always to call this in the regular executor
 * context, not in a shorter-lived context; else the temp file buffers
 * will get messed up.
 */
void
ExecHashJoinSaveTuple(HeapTuple heapTuple, uint32 hashvalue,
					  BufFile **fileptr)
{
	BufFile    *file = *fileptr;
	size_t		written;

	if (file == NULL)
	{
		/* First write to this batch file, so open it. */
		file = BufFileCreateTemp(false);
		*fileptr = file;
	}

	written = BufFileWrite(file, (void *) &hashvalue, sizeof(uint32));
	if (written != sizeof(uint32))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to hash-join temporary file: %m")));

	written = BufFileWrite(file, (void *) heapTuple, sizeof(HeapTupleData));
	if (written != sizeof(HeapTupleData))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to hash-join temporary file: %m")));

	written = BufFileWrite(file, (void *) heapTuple->t_data, heapTuple->t_len);
	if (written != (size_t) heapTuple->t_len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to hash-join temporary file: %m")));
}

/*
 * ExecHashJoinGetSavedTuple
 *		read the next tuple from a batch file.	Return NULL if no more.
 *
 * On success, *hashvalue is set to the tuple's hash value, and the tuple
 * itself is stored in the given slot.
 */
static TupleTableSlot *
ExecHashJoinGetSavedTuple(HashJoinState *hjstate,
						  BufFile *file,
						  uint32 *hashvalue,
						  TupleTableSlot *tupleSlot)
{
	HeapTupleData htup;
	size_t		nread;
	HeapTuple	heapTuple;

	nread = BufFileRead(file, (void *) hashvalue, sizeof(uint32));
	if (nread == 0)
		return NULL;			/* end of file */
	if (nread != sizeof(uint32))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from hash-join temporary file: %m")));
	nread = BufFileRead(file, (void *) &htup, sizeof(HeapTupleData));
	if (nread != sizeof(HeapTupleData))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from hash-join temporary file: %m")));
	heapTuple = palloc(HEAPTUPLESIZE + htup.t_len);
	memcpy((char *) heapTuple, (char *) &htup, sizeof(HeapTupleData));
	heapTuple->t_datamcxt = CurrentMemoryContext;
	heapTuple->t_data = (HeapTupleHeader)
		((char *) heapTuple + HEAPTUPLESIZE);
	nread = BufFileRead(file, (void *) heapTuple->t_data, htup.t_len);
	if (nread != (size_t) htup.t_len)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read from hash-join temporary file: %m")));
	return ExecStoreTuple(heapTuple, tupleSlot, InvalidBuffer, true);
}


void
ExecReScanHashJoin(HashJoinState *node, ExprContext *exprCtxt)
{
	/*
	 * In a multi-batch join, we currently have to do rescans the hard way,
	 * primarily because batch temp files may have already been released. But
	 * if it's a single-batch join, and there is no parameter change for the
	 * inner subnode, then we can just re-use the existing hash table without
	 * rebuilding it.
	 */
	if (node->hj_HashTable != NULL)
	{
		if (node->hj_HashTable->nbatch == 1 &&
			((PlanState *) node)->righttree->chgParam == NULL)
		{
			/*
			 * okay to reuse the hash table; needn't rescan inner, either.
			 *
			 * What we do need to do is reset our state about the emptiness
			 * of the outer relation, so that the new scan of the outer will
			 * update it correctly if it turns out to be empty this time.
			 * (There's no harm in clearing it now because ExecHashJoin won't
			 * need the info.  In the other cases, where the hash table
			 * doesn't exist or we are destroying it, we leave this state
			 * alone because ExecHashJoin will need it the first time
			 * through.)
			 */
			node->hj_OuterNotEmpty = false;
		}
		else
		{
			/* must destroy and rebuild hash table */
			ExecHashTableDestroy(node->hj_HashTable);
			node->hj_HashTable = NULL;

			/*
			 * if chgParam of subnode is not null then plan will be re-scanned
			 * by first ExecProcNode.
			 */
			if (((PlanState *) node)->righttree->chgParam == NULL)
				ExecReScan(((PlanState *) node)->righttree, exprCtxt);
		}
	}

	/* Always reset intra-tuple state */
	node->hj_CurHashValue = 0;
	node->hj_CurBucketNo = 0;
	node->hj_CurTuple = NULL;

	node->js.ps.ps_OuterTupleSlot = NULL;
	node->js.ps.ps_TupFromTlist = false;
	node->hj_NeedNewOuter = true;
	node->hj_MatchedOuter = false;
	node->hj_FirstOuterTupleSlot = NULL;

	node->hj_InnerFinished = false;
	node->hj_OuterFinished = false;

	node->hj_ReadInnerNext = true;

	node->hj_InnerMatchCount = 0;
	node->hj_OuterMatchCount = 0;

	/*
	 * if chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (((PlanState *) node)->lefttree->chgParam == NULL)
		ExecReScan(((PlanState *) node)->lefttree, exprCtxt);
}
