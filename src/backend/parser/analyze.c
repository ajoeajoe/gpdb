/*-------------------------------------------------------------------------
 *
 * analyze.c--
 *	  transform the parse tree into a query tree
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/parser/analyze.c,v 1.57 1997/12/27 06:41:26 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "postgres.h"
#include "access/heapam.h"
#include "nodes/makefuncs.h"
#include "nodes/memnodes.h"
#include "nodes/pg_list.h"
#include "parser/analyze.h"
#include "parser/parse_agg.h"
#include "parser/parse_clause.h"
#include "parser/parse_node.h"
#include "parser/parse_relation.h"
#include "parser/parse_target.h"
#include "utils/builtins.h"
#include "utils/mcxt.h"

static Query *transformStmt(ParseState *pstate, Node *stmt);
static Query *transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt);
static Query *transformInsertStmt(ParseState *pstate, AppendStmt *stmt);
static Query *transformIndexStmt(ParseState *pstate, IndexStmt *stmt);
static Query *transformExtendStmt(ParseState *pstate, ExtendStmt *stmt);
static Query *transformRuleStmt(ParseState *query, RuleStmt *stmt);
static Query *transformSelectStmt(ParseState *pstate, RetrieveStmt *stmt);
static Query *transformUpdateStmt(ParseState *pstate, ReplaceStmt *stmt);
static Query *transformCursorStmt(ParseState *pstate, CursorStmt *stmt);
static Query *transformCreateStmt(ParseState *pstate, CreateStmt *stmt);

List   *extras = NIL;

/*
 * parse_analyze -
 *	  analyze a list of parse trees and transform them if necessary.
 *
 * Returns a list of transformed parse trees. Optimizable statements are
 * all transformed to Query while the rest stays the same.
 *
 * CALLER is responsible for freeing the QueryTreeList* returned
 */
QueryTreeList *
parse_analyze(List *pl)
{
	QueryTreeList *result;
	ParseState *pstate;
	int			i = 0;

	result = malloc(sizeof(QueryTreeList));
	result->len = length(pl);
	result->qtrees = (Query **) malloc(result->len * sizeof(Query *));

	while (pl != NIL)
	{
		pstate = make_parsestate();
		result->qtrees[i++] = transformStmt(pstate, lfirst(pl));
		if (extras != NIL)
		{
			result->len += length(extras);
			result->qtrees = (Query **) realloc(result->qtrees, result->len * sizeof(Query *));
			while (extras != NIL)
			{
				result->qtrees[i++] = transformStmt(pstate, lfirst(extras));
				extras = lnext(extras);
			}
		}
		extras = NIL;
		pl = lnext(pl);
		if (pstate->p_target_relation != NULL)
			heap_close(pstate->p_target_relation);
		free(pstate);
	}

	return result;
}

/*
 * transformStmt -
 *	  transform a Parse tree. If it is an optimizable statement, turn it
 *	  into a Query tree.
 */
static Query *
transformStmt(ParseState *pstate, Node *parseTree)
{
	Query	   *result = NULL;

	switch (nodeTag(parseTree))
	{
			/*------------------------
			 *	Non-optimizable statements
			 *------------------------
			 */
		case T_CreateStmt:
			result = transformCreateStmt(pstate, (CreateStmt *) parseTree);
			break;

		case T_IndexStmt:
			result = transformIndexStmt(pstate, (IndexStmt *) parseTree);
			break;

		case T_ExtendStmt:
			result = transformExtendStmt(pstate, (ExtendStmt *) parseTree);
			break;

		case T_RuleStmt:
			result = transformRuleStmt(pstate, (RuleStmt *) parseTree);
			break;

		case T_ViewStmt:
			{
				ViewStmt   *n = (ViewStmt *) parseTree;

				n->query = (Query *) transformStmt(pstate, (Node *) n->query);
				result = makeNode(Query);
				result->commandType = CMD_UTILITY;
				result->utilityStmt = (Node *) n;
			}
			break;

		case T_VacuumStmt:
			{
				MemoryContext oldcontext;

				/*
				 * make sure that this Query is allocated in TopMemory
				 * context because vacuum spans transactions and we don't
				 * want to lose the vacuum Query due to end-of-transaction
				 * free'ing
				 */
				oldcontext = MemoryContextSwitchTo(TopMemoryContext);
				result = makeNode(Query);
				result->commandType = CMD_UTILITY;
				result->utilityStmt = (Node *) parseTree;
				MemoryContextSwitchTo(oldcontext);
				break;

			}
		case T_ExplainStmt:
			{
				ExplainStmt *n = (ExplainStmt *) parseTree;

				result = makeNode(Query);
				result->commandType = CMD_UTILITY;
				n->query = transformStmt(pstate, (Node *) n->query);
				result->utilityStmt = (Node *) parseTree;
			}
			break;

			/*------------------------
			 *	Optimizable statements
			 *------------------------
			 */
		case T_AppendStmt:
			result = transformInsertStmt(pstate, (AppendStmt *) parseTree);
			break;

		case T_DeleteStmt:
			result = transformDeleteStmt(pstate, (DeleteStmt *) parseTree);
			break;

		case T_ReplaceStmt:
			result = transformUpdateStmt(pstate, (ReplaceStmt *) parseTree);
			break;

		case T_CursorStmt:
			result = transformCursorStmt(pstate, (CursorStmt *) parseTree);
			break;

		case T_RetrieveStmt:
			result = transformSelectStmt(pstate, (RetrieveStmt *) parseTree);
			break;

		default:

			/*
			 * other statments don't require any transformation-- just
			 * return the original parsetree
			 */
			result = makeNode(Query);
			result->commandType = CMD_UTILITY;
			result->utilityStmt = (Node *) parseTree;
			break;
	}
	return result;
}

/*
 * transformDeleteStmt -
 *	  transforms a Delete Statement
 */
static Query *
transformDeleteStmt(ParseState *pstate, DeleteStmt *stmt)
{
	Query	   *qry = makeNode(Query);

	qry->commandType = CMD_DELETE;

	/* set up a range table */
	makeRangeTable(pstate, stmt->relname, NULL);

	qry->uniqueFlag = NULL;

	/* fix where clause */
	qry->qual = transformWhereClause(pstate, stmt->whereClause);

	qry->rtable = pstate->p_rtable;
	qry->resultRelation = refnameRangeTablePosn(pstate->p_rtable, stmt->relname);

	/* make sure we don't have aggregates in the where clause */
	if (pstate->p_numAgg > 0)
		parseCheckAggregates(pstate, qry);

	return (Query *) qry;
}

/*
 * transformInsertStmt -
 *	  transform an Insert Statement
 */
static Query *
transformInsertStmt(ParseState *pstate, AppendStmt *stmt)
{
	Query	   *qry = makeNode(Query);	/* make a new query tree */
	List	   *icolumns;

	qry->commandType = CMD_INSERT;
	pstate->p_is_insert = true;

	/* set up a range table */
	makeRangeTable(pstate, stmt->relname, stmt->fromClause);

	qry->uniqueFlag = NULL;

	/* fix the target list */
	icolumns = pstate->p_insert_columns = makeTargetNames(pstate, stmt->cols);
	
	qry->targetList = transformTargetList(pstate, stmt->targetList);
	
	/* DEFAULT handling */
	if (length(qry->targetList) < pstate->p_target_relation->rd_att->natts &&
		pstate->p_target_relation->rd_att->constr &&
		pstate->p_target_relation->rd_att->constr->num_defval > 0)
	{
		AttributeTupleForm	   *att = pstate->p_target_relation->rd_att->attrs;
		AttrDefault			   *defval = pstate->p_target_relation->rd_att->constr->defval;
		int						ndef = pstate->p_target_relation->rd_att->constr->num_defval;
		
		/* 
		 * if stmt->cols == NIL then makeTargetNames returns list of all 
		 * attrs: have to shorter icolumns list...
		 */
		if (stmt->cols == NIL)
		{
			List   *extrl;
			int		i = length(qry->targetList);
			
			foreach (extrl, icolumns)
			{
				if (--i <= 0)
					break;
			}
			freeList (lnext(extrl));
			lnext(extrl) = NIL;
		}
		
		while (ndef-- > 0)
		{
			List		   *tl;
			Ident		   *id;
			TargetEntry	   *te;
			
			foreach (tl, icolumns)
			{
				id = (Ident *) lfirst(tl);
				if (!namestrcmp(&(att[defval[ndef].adnum - 1]->attname), id->name))
					break;
			}
			if (tl != NIL)		/* something given for this attr */
				continue;
			/* 
			 * Nothing given for this attr with DEFAULT expr, so
			 * add new TargetEntry to qry->targetList. 
			 * Note, that we set resno to defval[ndef].adnum:
			 * it's what transformTargetList()->make_targetlist_expr()
			 * does for INSERT ... SELECT. But for INSERT ... VALUES
			 * pstate->p_last_resno is used. It doesn't matter for 
			 * "normal" using (planner creates proper target list
			 * in preptlist.c), but may break RULEs in some way.
			 * It seems better to create proper target list here...
			 */
			te = makeNode(TargetEntry);
			te->resdom = makeResdom(defval[ndef].adnum,
									att[defval[ndef].adnum - 1]->atttypid,
									att[defval[ndef].adnum - 1]->attlen,
									pstrdup(nameout(&(att[defval[ndef].adnum - 1]->attname))),
									0, 0, 0);
			te->fjoin = NULL;
			te->expr = (Node *) stringToNode(defval[ndef].adbin);
			qry->targetList = lappend (qry->targetList, te);
		}
	}
	
	/* fix where clause */
	qry->qual = transformWhereClause(pstate, stmt->whereClause);

	/* now the range table will not change */
	qry->rtable = pstate->p_rtable;
	qry->resultRelation = refnameRangeTablePosn(pstate->p_rtable, stmt->relname);

	if (pstate->p_numAgg > 0)
		finalizeAggregates(pstate, qry);

	return (Query *) qry;
}

/* makeTableName()
 * Create a table name from a list of fields.
 */
static char *
makeTableName(void *elem,...);

static char *
makeTableName(void *elem,...)
{
	va_list	args;

	char   *name;
	char	buf[NAMEDATALEN+1];

	strcpy(buf,"");

	va_start(args,elem);

	name = elem;
	while (name != NULL)
	{
		/* not enough room for next part? then return nothing */
		if ((strlen(buf)+strlen(name)) >= (sizeof(buf)-1))
			return (NULL);

		if (strlen(buf) > 0) strcat(buf,"_");
		strcat(buf,name);

		name = va_arg(args,void *);
	}

	va_end(args);

	name = palloc(strlen(buf)+1);
	strcpy(name,buf);

	return (name);
} /* makeTableName() */

char *
CreateIndexName(char *tname, char *cname, char *label, List *indices);

char *
CreateIndexName(char *tname, char *cname, char *label, List *indices)
{
	int			pass = 0;
	char	   *iname = NULL;
	List	   *ilist;
	IndexStmt  *index;
	char		name2[NAMEDATALEN+1];

	/* use working storage, since we might be trying several possibilities */
	strcpy(name2,cname);
	while (iname == NULL)
	{
		iname = makeTableName(tname, name2, label, NULL);
		/* unable to make a name at all? then quit */
		if (iname == NULL)
			break;

#if PARSEDEBUG
printf("CreateNameIndex- check %s against indices\n",iname);
#endif

		ilist = indices;
		while (ilist != NIL)
		{
			index = lfirst(ilist);
#if PARSEDEBUG
printf("CreateNameIndex- compare %s with existing index %s\n",iname,index->idxname);
#endif
			if (strcasecmp(iname,index->idxname) == 0)
				break;

			ilist = lnext(ilist);
		}
		/* ran through entire list? then no name conflict found so done */
		if (ilist == NIL)
			break;

		/* the last one conflicted, so try a new name component */
		pfree(iname);
		iname = NULL;
		pass++;
		sprintf(name2, "%s_%d", cname, (pass+1));
	}

	return (iname);
} /* CreateIndexName() */

/*
 * transformCreateStmt -
 *	  transforms the "create table" statement
 *	  SQL92 allows constraints to be scattered all over, so thumb through
 *	   the columns and collect all constraints into one place.
 *	  If there are any implied indices (e.g. UNIQUE or PRIMARY KEY)
 *	   then expand those into multiple IndexStmt blocks.
 *	  - thomas 1997-12-02
 */
static Query *
transformCreateStmt(ParseState *pstate, CreateStmt *stmt)
{
	Query	   *q;
	int			have_pkey = FALSE;
	List	   *elements;
	Node	   *element;
	List	   *columns;
	List	   *dlist;
	ColumnDef  *column;
	List	   *constraints, *clist;
	Constraint *constraint;
	List	   *keys;
	Ident	   *key;
	List	   *ilist;
	IndexStmt  *index;
	IndexElem  *iparam;

	q = makeNode(Query);
	q->commandType = CMD_UTILITY;

	elements = stmt->tableElts;
	constraints = stmt->constraints;
	columns = NIL;
	dlist = NIL;

	while (elements != NIL)
	{
		element = lfirst(elements);
		switch (nodeTag(element))
		{
			case T_ColumnDef:
				column = (ColumnDef *) element;
#if PARSEDEBUG
printf("transformCreateStmt- found column %s\n",column->colname);
#endif
				columns = lappend(columns,column);
				if (column->constraints != NIL)
				{
#if PARSEDEBUG
printf("transformCreateStmt- found constraint(s) on column %s\n",column->colname);
#endif
					clist = column->constraints;
					while (clist != NIL)
					{
						constraint = lfirst(clist);
						switch (constraint->contype)
						{
							case CONSTR_NOTNULL:
#if PARSEDEBUG
printf("transformCreateStmt- found NOT NULL constraint on column %s\n",column->colname);
#endif
								if (column->is_not_null)
									elog(WARN,"CREATE TABLE/NOT NULL already specified"
										" for %s.%s", stmt->relname, column->colname);
								column->is_not_null = TRUE;
								break;

							case CONSTR_DEFAULT:
#if PARSEDEBUG
printf("transformCreateStmt- found DEFAULT clause on column %s\n",column->colname);
#endif
								if (column->defval != NULL)
									elog(WARN,"CREATE TABLE/DEFAULT multiple values specified"
										" for %s.%s", stmt->relname, column->colname);
								column->defval = constraint->def;
								break;

							case CONSTR_PRIMARY:
#if PARSEDEBUG
printf("transformCreateStmt- found PRIMARY KEY clause on column %s\n",column->colname);
#endif
								if (constraint->name == NULL)
									constraint->name = makeTableName(stmt->relname, "pkey", NULL);
								if (constraint->keys == NIL)
									constraint->keys = lappend(constraint->keys, column);
								dlist = lappend(dlist, constraint);
								break;

							case CONSTR_UNIQUE:
#if PARSEDEBUG
printf("transformCreateStmt- found UNIQUE clause on column %s\n",column->colname);
#endif
								if (constraint->name == NULL)
									constraint->name = makeTableName(stmt->relname, column->colname, "key", NULL);
								if (constraint->keys == NIL)
									constraint->keys = lappend(constraint->keys, column);
								dlist = lappend(dlist, constraint);
								break;

							case CONSTR_CHECK:
#if PARSEDEBUG
printf("transformCreateStmt- found CHECK clause on column %s\n",column->colname);
#endif
								constraints = lappend(constraints, constraint);
								if (constraint->name == NULL)
									constraint->name = makeTableName(stmt->relname, column->colname, NULL);
								break;

							default:
								elog(WARN,"parser: internal error; unrecognized constraint",NULL);
								break;
						}
						clist = lnext(clist);
					}
				}
				break;

			case T_Constraint:
				constraint = (Constraint *) element;
#if PARSEDEBUG
printf("transformCreateStmt- found constraint %s\n", ((constraint->name != NULL)? constraint->name: "(unknown)"));
#endif
				switch (constraint->contype)
				{
					case CONSTR_PRIMARY:
#if PARSEDEBUG
printf("transformCreateStmt- found PRIMARY KEY clause\n");
#endif
						if (constraint->name == NULL)
							constraint->name = makeTableName(stmt->relname, "pkey", NULL);
						dlist = lappend(dlist, constraint);
						break;

					case CONSTR_UNIQUE:
#if PARSEDEBUG
printf("transformCreateStmt- found UNIQUE clause\n");
#endif
#if FALSE
						if (constraint->name == NULL)
							constraint->name = makeTableName(stmt->relname, "key", NULL);
#endif
						dlist = lappend(dlist, constraint);
						break;

					case CONSTR_CHECK:
#if PARSEDEBUG
printf("transformCreateStmt- found CHECK clause\n");
#endif
						constraints = lappend(constraints, constraint);
						break;

					case CONSTR_NOTNULL:
					case CONSTR_DEFAULT:
						elog(WARN,"parser: internal error; illegal context for constraint",NULL);
						break;
					default:
						elog(WARN,"parser: internal error; unrecognized constraint",NULL);
						break;
				}
				break;

			default:
				elog(WARN,"parser: internal error; unrecognized node",NULL);
		}

		elements = lnext(elements);
	}

	stmt->tableElts = columns;
	stmt->constraints = constraints;

/* Now run through the "deferred list" to complete the query transformation.
 * For PRIMARY KEYs, mark each column as NOT NULL and create an index.
 * For UNIQUE, create an index as for PRIMARY KEYS, but do not insist on NOT NULL.
 *
 * Note that this code does not currently look for all possible redundant cases
 *  and either ignore or stop with warning. The create might fail later when
 *  names for indices turn out to be redundant, or a user might have specified
 *  extra useless indices which might hurt performance. - thomas 1997-12-08
 */
	ilist = NIL;
	while (dlist != NIL)
	{
		constraint = lfirst(dlist);
		if (nodeTag(constraint) != T_Constraint)
			elog(WARN,"parser: internal error; unrecognized deferred node",NULL);

#if PARSEDEBUG
printf("transformCreateStmt- found deferred constraint %s\n",
 ((constraint->name != NULL)? constraint->name: "(unknown)"));
#endif

		if (constraint->contype == CONSTR_PRIMARY)
			if (have_pkey)
				elog(WARN,"CREATE TABLE/PRIMARY KEY multiple primary keys"
					" for table %s are not legal", stmt->relname);
			else 
				have_pkey = TRUE;
		else if (constraint->contype != CONSTR_UNIQUE)
			elog(WARN,"parser: internal error; unrecognized deferred constraint",NULL);

#if PARSEDEBUG
printf("transformCreateStmt- found deferred %s clause\n",
 (constraint->contype == CONSTR_PRIMARY? "PRIMARY KEY": "UNIQUE"));
#endif

		index = makeNode(IndexStmt);

		index->unique = TRUE;
		if (constraint->name != NULL)
			index->idxname = constraint->name;
		else if (constraint->contype == CONSTR_PRIMARY)
		{
			if (have_pkey)
				elog(WARN,"CREATE TABLE/PRIMARY KEY multiple keys for table %s are not legal", stmt->relname);

			have_pkey = TRUE;
			index->idxname = makeTableName(stmt->relname, "pkey", NULL);
		}
		else
			index->idxname = NULL;

		index->relname = stmt->relname;
		index->accessMethod = "btree";
		index->indexParams = NIL;
		index->withClause = NIL;
		index->whereClause = NULL;
 
		keys = constraint->keys;
		while (keys != NIL)
		{
			key = lfirst(keys);
#if PARSEDEBUG
printf("transformCreateStmt- check key %s for column match\n", key->name);
#endif
			columns = stmt->tableElts;
			column = NULL;
			while (columns != NIL)
			{
				column = lfirst(columns);
#if PARSEDEBUG
printf("transformCreateStmt- check column %s for key match\n", column->colname);
#endif
				if (strcasecmp(column->colname,key->name) == 0) break;
				else column = NULL;
				columns = lnext(columns);
			}
			if (column == NULL)
				elog(WARN,"parser: column '%s' in key does not exist",key->name);

			if (constraint->contype == CONSTR_PRIMARY)
			{
#if PARSEDEBUG
printf("transformCreateStmt- mark column %s as NOT NULL\n", column->colname);
#endif
				column->is_not_null = TRUE;
			}
			iparam = makeNode(IndexElem);
			iparam->name = strcpy(palloc(strlen(column->colname)+1), column->colname);
			iparam->args = NIL;
			iparam->class = NULL;
			iparam->tname = NULL;
			index->indexParams = lappend(index->indexParams, iparam);

			if (index->idxname == NULL)
				index->idxname = CreateIndexName(stmt->relname, iparam->name, "key", ilist);

			keys = lnext(keys);
		}

		if (index->idxname == NULL)
			elog(WARN,"parser: unable to construct implicit index for table %s"
				"; name too long", stmt->relname);
		else
			elog(NOTICE,"CREATE TABLE/%s will create implicit index %s for table %s",
				((constraint->contype == CONSTR_PRIMARY)? "PRIMARY KEY": "UNIQUE"),
				index->idxname, stmt->relname);

		ilist = lappend(ilist, index);
		dlist = lnext(dlist);
	}

	q->utilityStmt = (Node *) stmt;
	extras = ilist;

	return q;
} /* transformCreateStmt() */

/*
 * transformIndexStmt -
 *	  transforms the qualification of the index statement
 */
static Query *
transformIndexStmt(ParseState *pstate, IndexStmt *stmt)
{
	Query	   *q;

	q = makeNode(Query);
	q->commandType = CMD_UTILITY;

	/* take care of the where clause */
	stmt->whereClause = transformWhereClause(pstate, stmt->whereClause);
	stmt->rangetable = pstate->p_rtable;

	q->utilityStmt = (Node *) stmt;

	return q;
}

/*
 * transformExtendStmt -
 *	  transform the qualifications of the Extend Index Statement
 *
 */
static Query *
transformExtendStmt(ParseState *pstate, ExtendStmt *stmt)
{
	Query	   *q;

	q = makeNode(Query);
	q->commandType = CMD_UTILITY;

	/* take care of the where clause */
	stmt->whereClause = transformWhereClause(pstate, stmt->whereClause);
	stmt->rangetable = pstate->p_rtable;

	q->utilityStmt = (Node *) stmt;
	return q;
}

/*
 * transformRuleStmt -
 *	  transform a Create Rule Statement. The actions is a list of parse
 *	  trees which is transformed into a list of query trees.
 */
static Query *
transformRuleStmt(ParseState *pstate, RuleStmt *stmt)
{
	Query	   *q;
	List	   *actions;

	q = makeNode(Query);
	q->commandType = CMD_UTILITY;

	actions = stmt->actions;

	/*
	 * transform each statment, like parse_analyze()
	 */
	while (actions != NIL)
	{

		/*
		 * NOTE: 'CURRENT' must always have a varno equal to 1 and 'NEW'
		 * equal to 2.
		 */
		addRangeTableEntry(pstate, stmt->object->relname, "*CURRENT*",
						   FALSE, FALSE);
		addRangeTableEntry(pstate, stmt->object->relname, "*NEW*",
						   FALSE, FALSE);

		pstate->p_last_resno = 1;
		pstate->p_is_rule = true;		/* for expand all */
		pstate->p_numAgg = 0;
		pstate->p_aggs = NULL;

		lfirst(actions) = transformStmt(pstate, lfirst(actions));
		actions = lnext(actions);
	}

	/* take care of the where clause */
	stmt->whereClause = transformWhereClause(pstate, stmt->whereClause);

	q->utilityStmt = (Node *) stmt;
	return q;
}


/*
 * transformSelectStmt -
 *	  transforms a Select Statement
 *
 */
static Query *
transformSelectStmt(ParseState *pstate, RetrieveStmt *stmt)
{
	Query	   *qry = makeNode(Query);

	qry->commandType = CMD_SELECT;

	/* set up a range table */
	makeRangeTable(pstate, NULL, stmt->fromClause);

	qry->uniqueFlag = stmt->unique;

	qry->into = stmt->into;
	qry->isPortal = FALSE;

	/* fix the target list */
	qry->targetList = transformTargetList(pstate, stmt->targetList);

	/* fix where clause */
	qry->qual = transformWhereClause(pstate, stmt->whereClause);

	/* check having clause */
	if (stmt->havingClause)
		elog(NOTICE, "HAVING not yet supported; ignore clause", NULL);

	/* fix order clause */
	qry->sortClause = transformSortClause(pstate,
										  stmt->sortClause,
										  qry->targetList,
										  qry->uniqueFlag);

	qry->groupClause = transformGroupClause(pstate,
											stmt->groupClause,
											qry->targetList);
	qry->rtable = pstate->p_rtable;

	if (pstate->p_numAgg > 0)
		finalizeAggregates(pstate, qry);

	qry->unionall = stmt->unionall;	/* in child, so unionClause may be false */
	
	if (stmt->unionClause)
	{
		List *ulist = NIL;
		QueryTreeList *qlist;
		int i, last_union = -1;
		bool union_all_found = false, union_found = false;

		qlist = parse_analyze(stmt->unionClause);

		/*
		 *	Do we need to split up our unions because we have UNION
		 *	and UNION ALL?
		 */
		for (i=0; i < qlist->len; i++)
		{
			if (qlist->qtrees[i]->unionall)
				union_all_found = true;
			else
			{
				union_found = true;
				last_union = i;
			}
		}

		/*	A trailing UNION negates the affect of earlier UNION ALLs */
		if (!union_all_found ||
			!union_found ||
			/* last entry is a UNION */
			!qlist->qtrees[qlist->len-1]->unionall)
		{
			for (i=0; i < qlist->len; i++)
				ulist = lappend(ulist, qlist->qtrees[i]);
			qry->unionClause = ulist;
		}
		else
		{
			List *union_list = NIL;
			Query *hold_qry;

			/*
			 *	We have mixed unions and non-unions, so we concentrate on
			 *	the last UNION in the list.
			 */
			for (i=0; i <= last_union; i++)
			{
				qlist->qtrees[i]->unionall = false;	/*make queries consistent*/
				union_list = lappend(union_list, qlist->qtrees[i]);
			}

			/*
			 *	Make the first UNION ALL after the last UNION our new
			 *	top query
			 */
			hold_qry = qry;
			qry = qlist->qtrees[last_union + 1];
			qry->unionClause = lcons(hold_qry, NIL); /* UNION queries */
			hold_qry->unionall = true;  /* UNION ALL this into other queries */
			hold_qry->unionClause = union_list;
			
			/*
			 *	The first UNION ALL after the last UNION is our anchor,
			 *	we skip it.
			 */
			for (i=last_union + 2; i < qlist->len; i++)
				/* all queries are UNION ALL */
				qry->unionClause = lappend(qry->unionClause, qlist->qtrees[i]);
		}
	}
	else
	    qry->unionClause = NULL;

	return (Query *) qry;
}

/*
 * transformUpdateStmt -
 *	  transforms an update statement
 *
 */
static Query *
transformUpdateStmt(ParseState *pstate, ReplaceStmt *stmt)
{
	Query	   *qry = makeNode(Query);

	qry->commandType = CMD_UPDATE;
	pstate->p_is_update = true;

	/*
	 * the FROM clause is non-standard SQL syntax. We used to be able to
	 * do this with REPLACE in POSTQUEL so we keep the feature.
	 */
	makeRangeTable(pstate, stmt->relname, stmt->fromClause);

	/* fix the target list */
	qry->targetList = transformTargetList(pstate, stmt->targetList);

	/* fix where clause */
	qry->qual = transformWhereClause(pstate, stmt->whereClause);

	qry->rtable = pstate->p_rtable;
	qry->resultRelation = refnameRangeTablePosn(pstate->p_rtable, stmt->relname);

	/* make sure we don't have aggregates in the where clause */
	if (pstate->p_numAgg > 0)
		parseCheckAggregates(pstate, qry);

	return (Query *) qry;
}

/*
 * transformCursorStmt -
 *	  transform a Create Cursor Statement
 *
 */
static Query *
transformCursorStmt(ParseState *pstate, CursorStmt *stmt)
{
	Query	   *qry = makeNode(Query);

	/*
	 * in the old days, a cursor statement is a 'retrieve into portal'; If
	 * you change the following, make sure you also go through the code in
	 * various places that tests the kind of operation.
	 */
	qry->commandType = CMD_SELECT;

	/* set up a range table */
	makeRangeTable(pstate, NULL, stmt->fromClause);

	qry->uniqueFlag = stmt->unique;

	qry->into = stmt->portalname;
	qry->isPortal = TRUE;
	qry->isBinary = stmt->binary;		/* internal portal */

	/* fix the target list */
	qry->targetList = transformTargetList(pstate, stmt->targetList);

	/* fix where clause */
	qry->qual = transformWhereClause(pstate, stmt->whereClause);

	/* fix order clause */
	qry->sortClause = transformSortClause(pstate,
										  stmt->sortClause,
										  qry->targetList,
										  qry->uniqueFlag);
	/* fix group by clause */
	qry->groupClause = transformGroupClause(pstate,
											stmt->groupClause,
											qry->targetList);

	qry->rtable = pstate->p_rtable;

	if (pstate->p_numAgg > 0)
		finalizeAggregates(pstate, qry);

	return (Query *) qry;
}
