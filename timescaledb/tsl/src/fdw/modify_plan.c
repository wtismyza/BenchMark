/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#include <postgres.h>
#include <parser/parsetree.h>
#include <access/sysattr.h>
#include <utils/rel.h>

#include <chunk.h>
#include <chunk_data_node.h>

#include "deparse.h"
#include "modify_plan.h"

static List *
get_insert_attrs(Relation rel)
{
	TupleDesc tupdesc = RelationGetDescr(rel);
	List *attrs = NIL;
	int i;

	for (i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i);

		if (!attr->attisdropped)
			attrs = lappend_int(attrs, AttrOffsetGetAttrNumber(i));
	}

	return attrs;
}

static List *
get_update_attrs(RangeTblEntry *rte)
{
	List *attrs = NIL;
	int col = -1;

	while ((col = bms_next_member(rte->updatedCols, col)) >= 0)
	{
		/* bit numbers are offset by FirstLowInvalidHeapAttributeNumber */
		AttrNumber attno = col + FirstLowInvalidHeapAttributeNumber;

		if (attno <= InvalidAttrNumber) /* shouldn't happen */
			elog(ERROR, "system-column update is not supported");

		attrs = lappend_int(attrs, attno);
	}

	return attrs;
}

static List *
get_chunk_data_nodes(Oid relid)
{
	Chunk *chunk = ts_chunk_get_by_relid(relid, false);
	List *serveroids = NIL;
	ListCell *lc;

	if (NULL == chunk)
		return NIL;

	foreach (lc, chunk->data_nodes)
	{
		ChunkDataNode *cs = lfirst(lc);

		serveroids = lappend_oid(serveroids, cs->foreign_server_oid);
	}

	return serveroids;
}

/*
 * Plan INSERT, UPDATE, and DELETE.
 *
 * The main task of this function is to generate (deparse) the SQL statement
 * for the corresponding tables on data nodes.
 *
 * If the planning involves a hypertable, the function is called differently
 * depending on the command:
 *
 * 1. INSERT - called only once during hypertable planning and the given
 * result relation is the hypertable root relation. This is due to
 * TimescaleDBs unique INSERT path. We'd like to plan the INSERT as if it
 * would happen on the root of the hypertable. This is useful because INSERTs
 * should occur via the top-level hypertables on the data nodes
 * (preferrably batched), and not once per individual remote chunk
 * (inefficient and won't go through the standard INSERT path on the data
 * node).
 *
 * 2. UPDATE and DELETE - called once per chunk and the given result relation
 * is the chunk relation.
 *
 * For non-hypertables, which are foreign tables using the timescaledb_fdw,
 * this function is called the way it normally would be for the FDW API, i.e.,
 * once during planning.
 *
 * For the TimescaleDB insert path, we actually call
 * this function only once on the hypertable's root table instead of once per
 * chunk. This is because we want to send INSERT statements to each remote
 * hypertable rather than each remote chunk.
 *
 * UPDATEs and DELETEs work slightly different since we have no "optimized"
 * path for such operations. Instead, they happen once per chunk.
 */
List *
fdw_plan_foreign_modify(PlannerInfo *root, ModifyTable *plan, Index result_relation,
						int subplan_index)
{
	CmdType operation = plan->operation;
	RangeTblEntry *rte = planner_rt_fetch(result_relation, root);
	Relation rel;
	StringInfoData sql;
	List *returning_list = NIL;
	List *retrieved_attrs = NIL;
	List *target_attrs = NIL;
	List *data_nodes = NIL;
	bool do_nothing = false;

	initStringInfo(&sql);

	/*
	 * Extract the relevant RETURNING list if any.
	 */
	if (plan->returningLists)
		returning_list = (List *) list_nth(plan->returningLists, subplan_index);

	/*
	 * ON CONFLICT DO UPDATE and DO NOTHING case with inference specification
	 * should have already been rejected in the optimizer, as presently there
	 * is no way to recognize an arbiter index on a foreign table.  Only DO
	 * NOTHING is supported without an inference specification.
	 */
	if (plan->onConflictAction == ONCONFLICT_NOTHING)
		do_nothing = true;
	else if (plan->onConflictAction != ONCONFLICT_NONE)
		elog(ERROR, "unexpected ON CONFLICT specification: %d", (int) plan->onConflictAction);

	/*
	 * Core code already has some lock on each rel being planned, so we can
	 * use NoLock here.
	 */
	rel = table_open(rte->relid, NoLock);

	/*
	 * Construct the SQL command string
	 *
	 * In an INSERT, we transmit all columns that are defined in the foreign
	 * table.  In an UPDATE, we transmit only columns that were explicitly
	 * targets of the UPDATE, so as to avoid unnecessary data transmission.
	 * (We can't do that for INSERT since we would miss sending default values
	 * for columns not listed in the source statement.)
	 */
	switch (operation)
	{
		case CMD_INSERT:
			target_attrs = get_insert_attrs(rel);
			deparseInsertSql(&sql,
							 rte,
							 result_relation,
							 rel,
							 target_attrs,
							 1,
							 do_nothing,
							 returning_list,
							 &retrieved_attrs);
			break;
		case CMD_UPDATE:
			target_attrs = get_update_attrs(rte);
			deparseUpdateSql(&sql,
							 rte,
							 result_relation,
							 rel,
							 target_attrs,
							 returning_list,
							 &retrieved_attrs);
			data_nodes = get_chunk_data_nodes(rel->rd_id);
			break;
		case CMD_DELETE:
			deparseDeleteSql(&sql, rte, result_relation, rel, returning_list, &retrieved_attrs);
			data_nodes = get_chunk_data_nodes(rel->rd_id);
			break;
		default:
			elog(ERROR, "unexpected operation: %d", (int) operation);
			break;
	}

	table_close(rel, NoLock);

	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match enum FdwModifyPrivateIndex, above.
	 */
	return list_make5(makeString(sql.data),
					  target_attrs,
					  makeInteger((retrieved_attrs != NIL)),
					  retrieved_attrs,
					  data_nodes);
}
