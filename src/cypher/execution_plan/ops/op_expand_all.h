/**
 * Copyright 2022 AntGroup CO., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

//
// Created by wt on 7/3/18.
//
#pragma once

#include "cypher/execution_plan/ops/op.h"
#include "filter/filter.h"

namespace cypher {

/* ExpandAll
 * Expands entire graph,
 * Each node within the graph will be set */
class ExpandAll : public OpBase {
    friend class EdgeFilterPushdownExpand;

    void _InitializeEdgeIter(RTContext *ctx) {
        auto &types = relp_->Types();
        auto iter_type = lgraph::EIter::NA;
        switch (expand_direction_) {
        case ExpandTowards::FORWARD:
            iter_type = types.empty() ? lgraph::EIter::OUT_EDGE : lgraph::EIter::TYPE_OUT_EDGE;
            break;
        case ExpandTowards::REVERSED:
            iter_type = types.empty() ? lgraph::EIter::IN_EDGE : lgraph::EIter::TYPE_IN_EDGE;
            break;
        case ExpandTowards::BIDIRECTIONAL:
            iter_type = types.empty() ? lgraph::EIter::BI_EDGE : lgraph::EIter::BI_TYPE_EDGE;
            break;
        }
        // eit_->Initialize(ctx->txn_->GetTxn().get(), iter_type, start_->PullVid(), types);

        // new append
        eit_->InitializeVersion(ctx->txn_->GetTxn().get(), iter_type, start_->PullVid(), types, versionl_, versionr_);
        // FMA_LOG() << eit_->Goto("test go to", start_->PullVid()+1);
    }

    // 使用过滤器
    // new append 删除const
    bool _CheckToSkipEdgeFilter(RTContext *ctx) {
        // if the query has edge_filter, filter before node_expand
        // return edge_filter_ && !edge_filter_->DoFilter(ctx, *children[0]->record);
        bool doFilterVersion = !edge_filter_->DoFilterVersion(ctx, *children[0]->record, &end_flag);
        return edge_filter_ && doFilterVersion;

    }

    // new append 删除const
    bool _CheckToSkipEdge(RTContext *ctx) {
        // if (eit_->IsValid())
            // FMA_LOG() << "@@@@@@@@@@@@ " << eit_->GetSrc() << " - " << eit_->GetDst() << " is checked";
        // pattern_graph_->VisitedEdges().Contains(*eit_)： 当前边已经被访问过了
        // _CheckToSkipEdgeFilter(ctx)：利用过滤器过滤
        return eit_->IsValid() &&
               (pattern_graph_->VisitedEdges().Contains(*eit_) || _CheckToSkipEdgeFilter(ctx) ||
                (expand_into_ && eit_->GetNbr(expand_direction_) != neighbor_->PullVid()));
    }

    // new append
    bool FilterByVersion() const {
        return false;
    }

    bool _FilterNeighborLabel(RTContext *ctx) {
        if (neighbor_->Label().empty()) {
            // FMA_LOG() << "label of neighbor is empty";
            return true;
        }
        // FMA_LOG() << "label of neighbor: " << neighbor_->Label();
        // FMA_LOG() << "current eit: " << eit_->GetSrc() << " - " << eit_->GetDst();
        // 先通过 eit_->GetNbr(expand_direction_) 获取邻居点的vid
        // 再通过 GetVertexIterator 构建邻居顶点的迭代器，包含了一个go to方法的调用
        auto nbr_it = ctx->txn_->GetTxn()->GetVertexIterator(eit_->GetNbr(expand_direction_));
        // FMA_LOG() << "Vid in nbr_it: "<< nbr_it.GetId();
        while (ctx->txn_->GetTxn()->GetVertexLabel(nbr_it) != neighbor_->Label()) {
            eit_->Next();
            if (!eit_->IsValid()) return false;
            nbr_it.Goto(eit_->GetNbr(expand_direction_));
            // FMA_LOG() << "Vid in nbr_it: "<< nbr_it.GetId();
            CYPHER_THROW_ASSERT(nbr_it.IsValid());
        }
        return true;
    }

    void _DumpForDebug() const {
#ifndef NDEBUG
        FMA_DBG() << "[" << __FILE__ << "] start:" << start_->PullVid()
                  << ", neighbor:" << neighbor_->PullVid();
        FMA_DBG() << pattern_graph_->VisitedEdges().Dump();
#endif
    }

    OpResult Next(RTContext *ctx) {
        // FMA_LOG() << "NEXT() in op_expand_all start";
        // Reset iterators
        if (state_ == ExpandAllResetted) {
            /* Start node iterator may be invalid, such as when the start is an argument
             * produced by OPTIONAL MATCH.  */
            // FMA_LOG() << "vid in op_expand_all: " << start_->PullVid();
            if (start_->PullVid() < 0) 
            {
                return OP_REFRESH;
            }
            // 初始化 边 迭代器
            _InitializeEdgeIter(ctx);
            // 提前过滤
            // FMA_LOG() << "===============test===============";
            //eit_->GotoVersion(start_->PullVid(), 0);
            // FMA_LOG() << "===============test===============";
            while (_CheckToSkipEdge(ctx)) {
                // FMA_LOG() << "_CheckToSkipEdge";
                eit_->Next();
            }
            if (!eit_->IsValid() || !_FilterNeighborLabel(ctx)) {
                return OP_REFRESH;
            }
            /* When relationship is undirected, GetNbr() will get src for out_edge_iterator
             * and dst for in_edge_iterator.  */
            neighbor_->PushVid(eit_->GetNbr(expand_direction_));
            pattern_graph_->VisitedEdges().Add(*eit_);
            state_ = ExpandAllConsuming;
            _DumpForDebug();
            return OP_OK;
        }
        // The iterators are set, keep on consuming.
        pattern_graph_->VisitedEdges().Erase(*eit_);
        do {
            if (end_flag)
                return OP_REFRESH;
            eit_->Next();
        } while (_CheckToSkipEdge(ctx));
        if (!eit_->IsValid() || !_FilterNeighborLabel(ctx)) {
            return OP_REFRESH;
        }
        neighbor_->PushVid(eit_->GetNbr(expand_direction_));
        pattern_graph_->VisitedEdges().Add(*eit_);
        _DumpForDebug();
        return OP_OK;
    }

 public:
    cypher::Node *start_;         // start node to expand
    cypher::Node *neighbor_;      // neighbor of start node
    cypher::Relationship *relp_;  // relationship to expand
    lgraph::EIter *eit_;
    int start_rec_idx_;
    int nbr_rec_idx_;
    int relp_rec_idx_;
    cypher::PatternGraph *pattern_graph_;
    bool expand_into_;
    ExpandTowards expand_direction_;
    std::shared_ptr<lgraph::Filter> edge_filter_ = nullptr;
    lgraph::VertexId versionl_;
    lgraph::VertexId versionr_;
    bool end_flag = false;

    /* ExpandAllStates
     * Different states in which ExpandAll can be at. */
    enum ExpandAllState {
        ExpandAllUninitialized, /* ExpandAll wasn't initialized it. */
        ExpandAllResetted,      /* ExpandAll was just restarted. */
        ExpandAllConsuming,     /* ExpandAll consuming data. */
    } state_;

    // TODO(anyone) rename expandAll to expand
    ExpandAll(PatternGraph *pattern_graph, Node *start, Node *neighbor, Relationship *relp,
              std::shared_ptr<lgraph::Filter> edge_filter = nullptr)
        : OpBase(OpType::EXPAND_ALL, "Expand"),
          start_(start),
          neighbor_(neighbor),
          relp_(relp),
          pattern_graph_(pattern_graph),
          edge_filter_(edge_filter) {
        CYPHER_THROW_ASSERT(start && neighbor && relp);
        // 构造边迭代器
        eit_ = relp->ItRef();
        modifies.emplace_back(neighbor_->Alias());
        modifies.emplace_back(relp_->Alias());
        auto &sym_tab = pattern_graph->symbol_table;
        auto sit = sym_tab.symbols.find(start_->Alias());
        auto nit = sym_tab.symbols.find(neighbor_->Alias());
        auto rit = sym_tab.symbols.find(relp_->Alias());
        CYPHER_THROW_ASSERT(sit != sym_tab.symbols.end() && nit != sym_tab.symbols.end() &&
                            rit != sym_tab.symbols.end());
        expand_into_ = nit->second.scope == SymbolNode::ARGUMENT;
        expand_direction_ = relp_->Undirected()            ? BIDIRECTIONAL
                            : relp_->Src() == start_->ID() ? FORWARD
                                                           : REVERSED;
        start_rec_idx_ = sit->second.id;
        nbr_rec_idx_ = nit->second.id;
        relp_rec_idx_ = rit->second.id;
        state_ = ExpandAllUninitialized;
    }

    void PushDownEdgeFilter(std::shared_ptr<lgraph::Filter> edge_filter) {
        edge_filter_ = edge_filter;
    }

    OpResult Initialize(RTContext *ctx) override {
        // FMA_LOG() << "initialization of op_expand_all is invoked";
        // 判断 edge_filter 是否为空
        if (edge_filter_){
            // FMA_LOG() << "filter is not null";
            // 判断 filter 类型是否为 RANGE_FILTER
            if (edge_filter_->Type() == lgraph::Filter::Type::RANGE_FILTER){
                FMA_LOG() << "edge_filter_ is a range filter";
                // 类型转换
                auto range_filter = std::dynamic_pointer_cast<lgraph::RangeFilter>(edge_filter_);
                // ae_right: 常量
                auto ae_right = range_filter->GetAeRight();
                if (ae_right.type == ArithExprNode::AR_EXP_OPERAND){
                    // FMA_LOG() << ae_right.operand.constant;
                    FMA_LOG() << "AR_EXP_OPERAND";
                    FMA_LOG() << ae_right.operand.constant.ToString();
                }
                else if (ae_right.type == ArithExprNode::AR_EXP_OP){
                    // FMA_LOG() << ae_right.operand.constant;
                    FMA_LOG() << "AR_EXP_OP";
                }
                else{
                    // FMA_LOG() << ae_right.operand.constant;
                    FMA_LOG() << "AR_AST_EXP";
                }
            }
            else if (edge_filter_->Type() == lgraph::Filter::Type::UNARY){
                FMA_LOG() << "edge_filter_ is a UNARY filter";
            }
            else if (edge_filter_->Type() == lgraph::Filter::Type::BINARY){
                // FMA_LOG() << "edge_filter_ is a BINARY filter";
                auto left_filter = std::dynamic_pointer_cast<lgraph::RangeFilter>(edge_filter_->Left());
                auto ae_right1 = left_filter->GetAeRight();
                versionl_ = std::stoll(ae_right1.operand.constant.ToString());
                // FMA_LOG() << "ae_right1: " << versionl_;
                auto right_filter = std::dynamic_pointer_cast<lgraph::RangeFilter>(edge_filter_->Right());
                auto ae_right2 = right_filter->GetAeRight();
                versionr_ = std::stoll(ae_right2.operand.constant.ToString());
                // FMA_LOG() << "ae_right2: " << versionr_;
                // FMA_LOG() << "left_filter: " << left_filter->GetAeRight().operand.constant.ToString() << "right_filter: " << right_filter->GetAeRight().operand.constant.ToString();
            }
            else if (edge_filter_->Type() == lgraph::Filter::Type::LABEL_FILTER){
                FMA_LOG() << "edge_filter_ is a LABEL_FILTER filter";
            }
            else if (edge_filter_->Type() == lgraph::Filter::Type::STRING_FILTER){
                FMA_LOG() << "edge_filter_ is a STRING_FILTER filter";
            }
            else if (edge_filter_->Type() == lgraph::Filter::Type::GEAX_EXPR_FILTER){
                FMA_LOG() << "edge_filter_ is a GEAX_EXPR_FILTER filter";
            }

        }
        CYPHER_THROW_ASSERT(!children.empty());
        auto child = children[0];
        auto res = child->Initialize(ctx);
        if (res != OP_OK) return res;
        record = child->record;
        record->values[start_rec_idx_].type = Entry::NODE;
        record->values[start_rec_idx_].node = start_;
        record->values[nbr_rec_idx_].type = Entry::NODE;
        record->values[nbr_rec_idx_].node = neighbor_;
        record->values[relp_rec_idx_].type = Entry::RELATIONSHIP;
        record->values[relp_rec_idx_].relationship = relp_;
        return OP_OK;
    }

    OpResult RealConsume(RTContext *ctx) override {
        // FMA_LOG() << "RealConsume (op_expand_all)";
        CYPHER_THROW_ASSERT(!children.empty());
        auto child = children[0];
        while (state_ == ExpandAllUninitialized || Next(ctx) == OP_REFRESH) {
            auto res = child->Consume(ctx);
            state_ = ExpandAllResetted;
            if (res != OP_OK) {
                /* When consume after the stream is DEPLETED, make sure
                 * the result always be DEPLETED.  */
                state_ = ExpandAllUninitialized;
                return res;
            }
            /* Most of the time, the start_it is definitely valid after child's Consume
             * returns OK, except when the child is an OPTIONAL operation.  */
        }
        return OP_OK;
    }

    OpResult ResetImpl(bool complete) override {
        /* TODO(anyone) optimize, the apply operator need reset rhs stream completely,
         * while the cartesian product doesn't.
         * e.g.:
         * match (n:Person {name:'Vanessa Redgrave'})-->(m) with m as m1
         * match (n:Person {name:'Vanessa Redgrave'})<--(m) return m as m2, m1
         * */
        /* reset modifies */
        eit_->FreeIter();
        neighbor_->PushVid(-1);
        pattern_graph_->VisitedEdges().Erase(*eit_);
        state_ = ExpandAllUninitialized;
        return OP_OK;
    }

    std::string ToString() const override {
        auto towards = expand_direction_ == FORWARD    ? "-->"
                       : expand_direction_ == REVERSED ? "<--"
                                                       : "--";
        std::string edgefilter_str = "EdgeFilter";
        return fma_common::StringFormatter::Format(
            "{}({}) [{} {} {} {}]", name, expand_into_ ? "Into" : "All", start_->Alias(), towards,
            neighbor_->Alias(),
            edge_filter_ ? edgefilter_str.append(" (").append(edge_filter_->ToString()).append(")")
                         : "");
    }

    Node* GetStartNode() const { return start_; }
    Node* GetNeighborNode() const { return neighbor_; }
    Relationship* GetRelationship() const { return relp_; }

    CYPHER_DEFINE_VISITABLE()

    CYPHER_DEFINE_CONST_VISITABLE()
};
}  // namespace cypher
