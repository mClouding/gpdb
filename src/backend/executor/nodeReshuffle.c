/*-------------------------------------------------------------------------
 *
 * nodeReshuffle.c
 *	  Support for reshuffling data in different segments size.
 *
 * DESCRIPTION
 *
 * 		Each table has a `numsegments` attribute in the
 * 		GP_DISTRIBUTION_POLICY table,  it indicates that the table's
 * 		data is distributed on the first N segments, In common case,
 * 		the `numsegments` equal the total segment count of this
 * 		cluster.
 *
 * 		When we add new segments into the cluster, `numsegments` no
 * 		longer equal the actual segment count in the cluster, we
 * 		need to reshuffle the table data to all segments in 2 steps:
 *
 * 			* Reshuffle the table data to all segments
 * 			* Update `numsegments`
 *
 * 		It is easy to update `numsegments`, so we focus on how to
 * 		reshuffle the table data, There are 3 type tables in the
 * 		Greenplum database, they are reshuffled in different ways.
 *
 * 		For hash distributed table, we reshuffle data based on
 * 		Update statement. Updating the hash keys of the	table
 *      will generate an Plan like:
 *
 * 			Update
 * 				->Redistributed Motion
 * 					->SplitUpdate
 * 						->SeqScan
 *
 * 		We can not use this Plan to reshuffle table data directly.
 * 		The problem is that we need to know the segment count
 * 		when Motion node computes the destination segment. When
 * 		we compute the destination segment of deleting tuple, it
 * 		need the old segment count which is equal `numsegments`;
 *      on the other hand, we need use the new segment count to
 *      compute the destination segment for	inserting tuple.
 *
 * 		So we have to add an new operator Reshuffle to compute the
 * 		destination segment, it record the O and N (O is the count
 * 		of old segments and N is the count of new segments), then
 * 		the Plan would be adjusted like:
 *
 * 			Update
 * 				->Explicit Motion
 * 					->Reshuffle
 * 						->SplitUpdate
 * 							->SeqScan
 *
 * 		It can compute the destination segments directly with O and
 * 		N, at the same time we change the Motion type to Explicit,
 * 		it can send tuple to the destination segment which we
 * 		computed in the Reshuffle node.
 *
 * 		With changing the hash method to the `jump hash`, not all
 * 		the table data need to reshuffle, so we add an new
 * 		ReshuffleExpr to filter the tuples which are need to
 * 		reshuffle, this expression will compute the destination
 * 		segment ahead of schedule, if the destination segment is
 * 		current segment, the tuple do not need to reshuffle, with
 * 		the ReshuffleExpr the plan would adjust like that:
 *
 * 			Update
 * 				->Explicit Motion
 * 					->Reshuffle
 * 						->SplitUpdate
 * 							->SeqScan
 * 								|-ReshuffleExpr
 *
 * 		When we want to reshuffle one table, we use the SQL `ALTER
 * 		TABLE xxx SET WITH (RESHUFFLE)`, Actually it will generate
 * 		an new UpdateStmt parse tree, the parse tree is similar to
 * 		the parse tree which is generated by SQL `UPDATE xxx SET
 * 		xxx.aaa = COALESCE(xxx.aaa...) WHERE ReshuffleExpr`. We set
 * 		an reshuffle flag in the UpdateStmt, so it can distinguish
 * 		the common update and the reshuffling.
 *
 * 		In conclusion, we reshuffle hash distributed table by
 * 		Reshuffle node and ReshuffleExpr, the ReshuffleExpr filter
 * 		the tuple need to reshuffle and the Reshuffle node do the
 * 		real reshuffling work, we can use that framework to
 * 		implement reshuffle random distributed table and replicated
 * 		table.
 *
 * 		For random distributed table, it have no hash keys,  each
 * 		old segment need reshuffle (O - N) / N data to the new
 * 		segments, In the ReshuffleExpr, we can generate a random
 * 		value between [0, N), if the random values is greater than
 * 		O, it means that the tuple need to reshuffle, so SeqScan
 * 		node can return this tuple to ReshuffleNode.  Reshuffle node
 * 		will generate an random value between [O, N), it means which
 * 		new segment the tuple need to insert.
 *
 * 		For replicated table, the table data is same in the all old
 * 		segments, so there do not need to delete any tuples, it only
 * 		need copy the tuple which is in the old segments to the new
 * 		segments, so the ReshuffleExpr do not filte any tuples, In
 * 		the Reshuffle node, we neglect the tuple which is generated
 * 		for deleting, only return the inserting tuple to motion. Let
 * 		me illustrate this with an example:
 *
 * 		If there are 3 old segments in the cluster and we add 4 new
 * 		segments, the segment ID of old segments is (0,1,2) and the
 * 		segment ID of new segments is (3,4,5,6), when reshuffle the
 * 		replicated table, the seg#0 is responsible to copy data to
 * 		seg#3 and seg#6, the seg#1 is responsible to copy data to
 * 		seg#4, the seg#2 is responsible to copy data to seg#5.
 *
 *
 * Portions Copyright (c) 2012, EMC Corp.
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeReshuffle.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "executor/executor.h"
#include "executor/nodeReshuffle.h"
#include "utils/memutils.h"

#include "cdb/cdbhash.h"
#include "cdb/cdbvars.h"
#include "cdb/memquota.h"
#include "cdb/cdbutil.h"

#define INIT_IDX 0

/*
 *  EvalHashSegID
 *
 * 	compute the Hash keys
 */
static int
EvalHashSegID(Datum *values, bool *nulls, List *policyAttrs, CdbHash *h)
{
	uint32		newSeg;
	ListCell   *lc;
	int			i;

	Assert(policyAttrs);

	cdbhashinit(h);

	i = 0;
	foreach(lc, policyAttrs)
	{
		AttrNumber attidx = lfirst_int(lc);

		cdbhash(h, i + 1, values[attidx - 1], nulls[attidx - 1]);
		i++;
	}

	newSeg = cdbhashreduce(h);

	return newSeg;
}

/* ----------------------------------------------------------------
 *		ExecReshuffle(node)
 *
 *  For hash distributed tables:
 *  	we compute the destination segment with Hash methods and
 *  	new segments count.
 *
 *  For random distributed tables:
 *  	we get an random value [0, newSeg# - oldSeg#), then the
 *  	destination segment is (random value + oldSeg#)
 *
 *  For replicated tables:
 *  	if there are 3 old segments in the cluster and we add 4
 *  	new segments:
 *  	old segments: 0,1,2
 *  	new segments: 3,4,5,6
 *  	the seg#0 is responsible to copy data to seg#3 and seg#6
 *  	the seg#1 is responsible to copy data to seg#4
 *  	the seg#2 is responsible to copy data to seg#5
 *
 * ----------------------------------------------------------------
 */
TupleTableSlot *
ExecReshuffle(ReshuffleState *node)
{
	PlanState *outerNode = outerPlanState(node);
	Reshuffle *reshuffle = (Reshuffle *) node->ps.plan;
	SplitUpdate *splitUpdate;

	TupleTableSlot *slot = NULL;

	Datum *values;
	bool *nulls;

	int dmlAction;

	Assert(outerNode != NULL);
	Assert(IsA(outerNode->plan, SplitUpdate));

	splitUpdate = (SplitUpdate *) outerNode->plan;

	Assert(splitUpdate->actionColIdx > 0);

	/* New added segments have no data */
	if (GpIdentity.segindex >= reshuffle->oldSegs)
		return NULL;

	if (reshuffle->ptype == POLICYTYPE_PARTITIONED)
	{
		slot = ExecProcNode(outerNode);

		if (TupIsNull(slot))
		{
			return NULL;
		}

		slot_getallattrs(slot);
		values = slot_get_values(slot);
		nulls = slot_get_isnull(slot);

		dmlAction = DatumGetInt32(values[splitUpdate->actionColIdx - 1]);

		Assert(dmlAction == DML_INSERT || dmlAction == DML_DELETE);

		if (DML_INSERT == dmlAction)
		{
			/* For hash distributed tables*/
			if (NULL != reshuffle->policyAttrs)
			{
				values[reshuffle->tupleSegIdx - 1] =
						Int32GetDatum(EvalHashSegID(values,
													nulls,
													reshuffle->policyAttrs,
													node->cdbhash));
			}
			else
			{
				/* For random distributed tables */
				int newSegs = getgpsegmentCount();
				int oldSegs = reshuffle->oldSegs;

				Assert(newSegs > oldSegs);

				/*
				 * Tuple with inserting action must be sent to other segments.
				 * Since the table is distributed randomly, we randomly pick one
				 * of the new segments[seg_Old, Seg_New) as target with uniform
				 * probability.
				 */
				values[reshuffle->tupleSegIdx - 1] =
					Int32GetDatum(oldSegs + random() % (newSegs - oldSegs));
			}
		}
#ifdef USE_ASSERT_CHECKING
		else
		{
			if (NULL != reshuffle->policyAttrs)
			{
				Datum oldSegID = values[reshuffle->tupleSegIdx - 1];
				Datum newSegID = Int32GetDatum(
						EvalHashSegID(values,
									  nulls,
									  reshuffle->policyAttrs,
									  node->oldcdbhash));

				Assert(oldSegID == newSegID);
			}
		}

		/* check */
		if (DatumGetInt32(values[reshuffle->tupleSegIdx - 1]) >=
			getgpsegmentCount())
			elog(ERROR, "ERROR SEGMENT ID : %d",
				 DatumGetInt32(values[reshuffle->tupleSegIdx - 1]));
#endif
	}
	else if (reshuffle->ptype == POLICYTYPE_REPLICATED)
	{
		int			segIdx;

		/* For replicated tables */
		if (GpIdentity.segindex + reshuffle->oldSegs >=
			getgpsegmentCount())
			return NULL;

		/*
		 * Each old segment can be responsible for copying data to
		 * more than one new segment.
		 */
		while(1)
		{
			if (node->newTargetIdx == INIT_IDX)
			{
				slot = ExecProcNode(outerNode);
				if (TupIsNull(slot))
					return NULL;

				/* It seems OK without deep copying the slot*/
				node->savedSlot = slot;
			}
			else
			{
				slot = node->savedSlot;
				Assert(!TupIsNull(slot));
			}

			slot_getallattrs(slot);
			values = slot_get_values(slot);

			dmlAction = DatumGetInt32(values[splitUpdate->actionColIdx - 1]);
			Assert(dmlAction == DML_INSERT || dmlAction == DML_DELETE);

			/* Reshuffling replicate table does not need to delete tuple */
			if (dmlAction == DML_DELETE)
				continue;

			/*
			 * Now we are handling inserting tuples and self_segid < N.
			 * N old segments(0, 1, 2, ... N-1)
			 * M new segments(N, N+1, ... N+M-1)
			 * The algorithm is that self_segid sends to the newsegments with id is
			 * self_segid + kN(k >= 1)
			 */
			segIdx = list_nth_int(node->destList, node->newTargetIdx);
			node->newTargetIdx ++;
			if (node->newTargetIdx >= list_length(node->destList))
			{
				node->newTargetIdx = INIT_IDX;
			}

			values[reshuffle->tupleSegIdx - 1] = Int32GetDatum(segIdx);
			break;
		}
	}
	else
	{
		/* Impossible case */
		Assert(false);
	}

	return slot;
}

/* ----------------------------------------------------------------
 *		ExecInitReshuffle
 *
 * ----------------------------------------------------------------
 */
ReshuffleState *
ExecInitReshuffle(Reshuffle *node, EState *estate, int eflags)
{
	ReshuffleState *reshufflestate;
	Oid		   *typeoids;
	int			i;
	ListCell   *lc;

	/* check for unsupported flags */
	Assert(!(eflags & (EXEC_FLAG_REWIND | EXEC_FLAG_MARK | EXEC_FLAG_BACKWARD)) ||
		   outerPlan(node) != NULL);

	/*
	 * create state structure
	 */
	reshufflestate = makeNode(ReshuffleState);
	reshufflestate->ps.plan = (Plan *) node;
	reshufflestate->ps.state = estate;

	/*
	 * initialize child expressions
	 */
	reshufflestate->ps.qual = (List *)
			ExecInitExpr((Expr *) node->plan.qual,
						 (PlanState *) reshufflestate);

	/*
	 * initialize child nodes
	 */
	outerPlanState(reshufflestate) = ExecInitNode(outerPlan(node), estate, eflags);

	/*
	 * we don't use inner plan
	 */
	Assert(innerPlan(node) == NULL);

	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &reshufflestate->ps);

	/*
	 * initialize tuple type and projection info
	 */
	ExecAssignResultTypeFromTL(&reshufflestate->ps);
	ExecAssignProjectionInfo(&reshufflestate->ps, NULL);

#if 0
	if (!IsResManagerMemoryPolicyNone()
		&& IsResultMemoryIntensive(node))
	{
		SPI_ReserveMemory(((Plan *)node)->operatorMemKB * 1024L);
	}
#endif

	/* Setup the destination segment ID list */
	if (!IS_QUERY_DISPATCHER())
	{
		if (GpIdentity.segindex < node->oldSegs &&
			GpIdentity.segindex + node->oldSegs < getgpsegmentCount())
		{
			int segIdx = GpIdentity.segindex + node->oldSegs;
			while (segIdx < getgpsegmentCount())
			{
				reshufflestate->destList =
						lappend_int(reshufflestate->destList, segIdx);
				segIdx = segIdx + node->oldSegs;
			}
		}
	}

	/* Initialize cdbhash objects */
	typeoids = palloc(list_length(node->policyAttrs) * sizeof(Oid));
	i = 0;
	foreach(lc, node->policyAttrs)
	{
		AttrNumber attidx = lfirst_int(lc);
		TargetEntry *entry = list_nth(node->plan.targetlist, attidx - 1);

		typeoids[i] = exprType((Node *) entry->expr);
		i++;
	}
	reshufflestate->cdbhash = makeCdbHash(getgpsegmentCount(), list_length(node->policyAttrs), typeoids);
#ifdef USE_ASSERT_CHECKING
	reshufflestate->oldcdbhash = makeCdbHash(node->oldSegs, list_length(node->policyAttrs), typeoids);
#endif

	reshufflestate->newTargetIdx = INIT_IDX;
	reshufflestate->savedSlot = NULL;

	return reshufflestate;
}

/* ----------------------------------------------------------------
 *		ExecEndReshuffle
 * ----------------------------------------------------------------
 */
void
ExecEndReshuffle(ReshuffleState *node)
{
	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);

	/*
	 * shut down subplans
	 */
	ExecEndNode(outerPlanState(node));

	EndPlanStateGpmonPkt(&node->ps);

	return;
}

/* ----------------------------------------------------------------
 *		ExecReScanReshuffle
 * ----------------------------------------------------------------
 */
void
ExecReScanReshuffle(ReshuffleState *node)
{
	/*
	 * If chgParam of subnode is not null then plan will be re-scanned by
	 * first ExecProcNode.
	 */
	if (node->ps.lefttree &&
		node->ps.lefttree->chgParam == NULL)
		ExecReScan(node->ps.lefttree);

	return;
}
