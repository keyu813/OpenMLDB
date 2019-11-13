/*-------------------------------------------------------------------------
 * Copyright (C) 2019, 4paradigm
 * planner.cc
 *
 * Author: chenjing
 * Date: 2019/10/24
 *--------------------------------------------------------------------------
 **/
#include "plan/planner.h"
#include <map>
#include <set>
#include <string>
namespace fesql {
namespace plan {

/**
 * create simple select plan node:
 *  simple select:
 *      + from_list
 *          + from_node
 *              + table_ref_node
 *      + project_list
 *          + project_node
 *              + expression
 *                  +   op_expr
 *                      | function
 *                      | const
 *                      | column ref node
 *              + name
 *          + project_node
 *          + project_node
 *          + ..
 *      + limit_count
 *
 * @param root
 * @return select plan node
 */

int Planner::CreateSelectPlan(const node::SQLNode *select_tree, PlanNode *plan_tree,
                              Status &status) {  // NOLINT (runtime/references)
    const node::SelectStmt *root = (const node::SelectStmt *)select_tree;
    node::SelectPlanNode *select_plan = (node::SelectPlanNode *)plan_tree;

    const node::NodePointVector& table_ref_list = root->GetTableRefList();

    if (table_ref_list.empty()) {
        status.msg =
            "can not create select plan node with empty table references";
        status.code = error::kPlanErrorTableRefIsEmpty;
        return status.code;
    }

    if (table_ref_list.size() > 1) {
        status.msg =
            "can not create select plan node based on more than 2 tables";
        status.code = error::kPlanErrorQueryMultiTable;
        return status.code;
    }

    const node::TableNode *table_node_ptr = (const node::TableNode *)table_ref_list.at(0);

    node::PlanNode *current_node = select_plan;

    std::map<std::string, node::ProjectListPlanNode *> project_list_map;
    // set limit
    if (nullptr != root->GetLimit()) {
        const node::LimitNode *limit_ptr = (node::LimitNode *)root->GetLimit();
        node::LimitPlanNode *limit_plan_ptr =
            (node::LimitPlanNode *)node_manager_->MakePlanNode(
                node::kPlanTypeLimit);
        limit_plan_ptr->SetLimitCnt(limit_ptr->GetLimitCount());
        current_node->AddChild(limit_plan_ptr);
        current_node = limit_plan_ptr;
    }

    // prepare project list plan node
    const node::NodePointVector& select_expr_list = root->GetSelectList();

    if (false == select_expr_list.empty()) {
        for (auto expr : select_expr_list) {
            node::ProjectPlanNode *project_node_ptr =
                (node::ProjectPlanNode *)(node_manager_->MakePlanNode(
                    node::kProject));

            CreateProjectPlanNode(expr, table_node_ptr->GetOrgTableName(),
                                  project_node_ptr, status);
            if (0 != status.code) {
                return status.code;
            }

            std::string key = project_node_ptr->GetW().empty()
                                  ? project_node_ptr->GetTable()
                                  : project_node_ptr->GetW();
            if (project_list_map.find(key) == project_list_map.end()) {
                project_list_map[key] =
                    project_node_ptr->GetW().empty()
                        ? node_manager_->MakeProjectListPlanNode(key, "")
                        : node_manager_->MakeProjectListPlanNode(
                              project_node_ptr->GetTable(), key);
            }
            project_list_map[key]->AddProject(project_node_ptr);
        }

        for (auto &v : project_list_map) {
            node::ProjectListPlanNode *project_list = v.second;
            project_list->AddChild(
                node_manager_->MakeSeqScanPlanNode(project_list->GetTable()));
            current_node->AddChild(v.second);
        }
    }

    return 0;
}

void Planner::CreateProjectPlanNode(
    const SQLNode *root, const std::string& table_name, node::ProjectPlanNode *plan_tree,
    Status &status) {  // NOLINT (runtime/references)
    if (nullptr == root) {
        status.msg = "fail to create project node: query tree node it null";
        status.code = error::kPlanErrorNullNode;
        return;
    }

    switch (root->GetType()) {
        case node::kResTarget: {
            const node::ResTarget *target_ptr = (const node::ResTarget *) root;
            std::string w = node::WindowOfExpression(target_ptr->GetVal());
            plan_tree->SetW(w);
            plan_tree->SetName(target_ptr->GetName());
            plan_tree->SetExpression(target_ptr->GetVal());
            plan_tree->SetTable(table_name);
            return;
        }
        default: {
            status.msg = "can not create project plan node with type " +
                         node::NameOfSQLNodeType(root->GetType());
            status.code = error::kPlanErrorUnSupport;
            return;
        }
    }
}


void Planner::CreateDataProviderPlanNode(
    const SQLNode *root, PlanNode *plan_tree,
    Status &status) {  // NOLINT (runtime/references)
}

void Planner::CreateDataCollectorPlanNode(
    const SQLNode *root, PlanNode *plan_tree,
    Status &status) {  // NOLINT (runtime/references)
}
void Planner::CreateCreateTablePlan(
    const node::SQLNode *root, node::CreatePlanNode *plan_tree,
    Status &status) {  // NOLINT (runtime/references)
    const node::CreateStmt *create_tree = (const node::CreateStmt *)root;
    plan_tree->SetColumnDescList(create_tree->GetColumnDefList());
    plan_tree->setTableName(create_tree->GetTableName());
}

int SimplePlanner::CreatePlanTree(
    const NodePointVector &parser_trees, PlanNodeList &plan_trees,
    Status &status) {  // NOLINT (runtime/references)
    if (parser_trees.empty()) {
        status.msg = "fail to create plan tree: parser trees is empty";
        status.code = error::kPlanErrorQueryTreeIsEmpty;
        return status.code;
    }

    for (auto parser_tree : parser_trees) {
        switch (parser_tree->GetType()) {
            case node::kSelectStmt: {
                PlanNode *select_plan =
                    node_manager_->MakePlanNode(node::kPlanTypeSelect);
                CreateSelectPlan(parser_tree, select_plan, status);
                if (0 != status.code) {
                    return status.code;
                }
                plan_trees.push_back(select_plan);
                break;
            }
            case node::kCreateStmt: {
                PlanNode *plan =
                    node_manager_->MakePlanNode(node::kPlanTypeCreate);
                CreateCreateTablePlan(
                    parser_tree, dynamic_cast<node::CreatePlanNode *>(plan),
                    status);
                if (0 != status.code) {
                    return status.code;
                }
                plan_trees.push_back(plan);
                break;
            }
            case node::kCmdStmt: {
                node::PlanNode *cmd_plan =
                    node_manager_->MakePlanNode(node::kPlanTypeCmd);
                CreateCmdPlan(parser_tree,
                              dynamic_cast<node::CmdPlanNode *>(cmd_plan),
                              status);
                if (0 != status.code) {
                    return status.code;
                }
                plan_trees.push_back(cmd_plan);
                break;
            }
            case ::fesql::node::kFnList:
             {
                 break;
             }
            default: {
                status.msg = "can not handle tree type " +
                             node::NameOfSQLNodeType(parser_tree->GetType());
                status.code = error::kPlanErrorUnSupport;
                return status.code;
            }
        }
    }
    return status.code;
}
void Planner::CreateCmdPlan(const SQLNode *root, node::CmdPlanNode *plan,
                                  Status &status) {
    if (nullptr == root) {
        status.msg = "fail to create cmd plan node: query tree node it null";
        status.code = error::kPlanErrorNullNode;
        return;
    }

    if (root->GetType() != node::kCmdStmt) {
        status.msg = "fail to create cmd plan node: query tree node it not cmd type";
        status.code = error::kPlanErrorUnSupport;
        return; 
    }

    plan->SetCmdNode(dynamic_cast<const node::CmdNode *>(root));
}

void TransformTableDef(const std::string &table_name,
                       const NodePointVector &column_desc_list,
                       type::TableDef *table,
                       Status &status) {  // NOLINT (runtime/references)
    std::set<std::string> index_names;
    std::set<std::string> column_names;

    for (auto column_desc : column_desc_list) {
        switch (column_desc->GetType()) {
            case node::kColumnDesc: {
                node::ColumnDefNode *column_def =
                    (node::ColumnDefNode *)column_desc;
                type::ColumnDef *column = table->add_columns();

                if (column_names.find(column_def->GetColumnName()) !=
                    column_names.end()) {
                    status.msg = "CREATE error: COLUMN NAME " +
                                 column_def->GetColumnName() + " duplicate";
                    status.code = error::kCreateErrorDuplicationColumnName;
                    return;
                }
                column->set_name(column_def->GetColumnName());
                column_names.insert(column_def->GetColumnName());
                switch (column_def->GetColumnType()) {
                    case node::kTypeBool:
                        column->set_type(type::Type::kBool);
                        break;
                    case node::kTypeInt32:
                        column->set_type(type::Type::kInt32);
                        break;
                    case node::kTypeInt64:
                        column->set_type(type::Type::kInt64);
                        break;
                    case node::kTypeFloat:
                        column->set_type(type::Type::kFloat);
                        break;
                    case node::kTypeDouble:
                        column->set_type(type::Type::kDouble);
                        break;
                    case node::kTypeTimestamp: {
                        column->set_type(type::Type::kTimestamp);
                        break;
                    }
                    case node::kTypeString:
                        column->set_type(type::Type::kString);
                        break;
                    default: {
                        status.msg =
                            "CREATE error: column type " +
                            node::DataTypeName(column_def->GetColumnType()) +
                            " is not supported";
                        status.code = error::kCreateErrorUnSupportColumnType;
                        return;
                    }
                }
                break;
            }

            case node::kColumnIndex: {
                node::ColumnIndexNode *column_index =
                    (node::ColumnIndexNode *)column_desc;

                if (column_index->GetName().empty()) {
                    column_index->SetName(
                        GenerateName("INDEX", table->indexes_size()));
                }
                if (index_names.find(column_index->GetName()) !=
                    index_names.end()) {
                    status.msg = "CREATE error: INDEX NAME " +
                                 column_index->GetName() + " duplicate";
                    status.code = error::kCreateErrorDuplicationIndexName;
                    return;
                }
                index_names.insert(column_index->GetName());
                type::IndexDef *index = table->add_indexes();
                index->set_name(column_index->GetName());

                // TODO(chenjing): set ttl per key
                if (-1 != column_index->GetTTL()) {
                    index->add_ttl(column_index->GetTTL());
                }

                for (auto key : column_index->GetKey()) {
                    index->add_first_keys(key);
                }

                if (!column_index->GetTs().empty()) {
                    index->set_second_key(column_index->GetTs());
                }
                break;
            }
            default: {
                status.msg = "can not support " +
                             node::NameOfSQLNodeType(column_desc->GetType()) +
                             " when CREATE TABLE";
                status.code = error::kAnalyserErrorUnSupport;
                return;
            }
        }
    }
    table->set_name(table_name);
}

std::string GenerateName(const std::string prefix, int id) {
    time_t t;
    time(&t);
    std::string name =
        prefix + "_" + std::to_string(id) + "_" + std::to_string(t);
    return name;
}

}  // namespace  plan
}  // namespace fesql
