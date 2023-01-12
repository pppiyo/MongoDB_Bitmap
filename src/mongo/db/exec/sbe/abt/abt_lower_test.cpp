/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/sbe/abt/abt_lower.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/explain.h"
#include "mongo/db/query/optimizer/metadata.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/node_defs.h"
#include "mongo/db/query/optimizer/props.h"
#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/rewrites/path_lower.h"
#include "mongo/db/query/optimizer/syntax/expr.h"
#include "mongo/db/query/optimizer/utils/unit_test_utils.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/unittest.h"
#include <vector>

namespace mongo::optimizer {
namespace {

unittest::GoldenTestConfig goldenTestConfig{"src/mongo/db/test_output/exec/sbe"};
using GoldenTestContext = unittest::GoldenTestContext;
using GoldenTestConfig = unittest::GoldenTestConfig;
class ABTPlanGeneration : public unittest::Test {
protected:
    ProjectionName scanLabel = ProjectionName{"scan0"_sd};
    NodeToGroupPropsMap _nodeMap;
    // This can be modified by tests that need other labels.
    FieldProjectionMap _fieldProjMap{{}, {scanLabel}, {}};

    void runExpressionVariation(GoldenTestContext& gctx, const std::string& name, const ABT& n) {
        auto& stream = gctx.outStream();
        if (stream.tellp()) {
            stream << std::endl;
        }
        stream << "==== VARIATION: " << name << " ====" << std::endl;
        stream << "-- INPUT:" << std::endl;
        stream << ExplainGenerator::explainV2(n) << std::endl;
        stream << "-- OUTPUT:" << std::endl;
        auto env = VariableEnvironment::build(n);
        SlotVarMap map;
        auto expr = SBEExpressionLowering{env, map}.optimize(n);
        stream << expr->toString() << std::endl;
    }

    // SBE plans with scans print UUIDs. As there are no collections in these tests the UUIDS
    // are generated by the ScanStage. Remove all UUIDs them so they don't throw off the test
    // output.
    std::string stripUUIDs(std::string str) {
        size_t atIndex = -1;  // size_t is unsigned, but we just want atIndex+1 == 0 in the first
                              // loop, so underflow/overflow is fine.
        while ((atIndex = str.find('@', atIndex + 1)) != std::string::npos) {
            // UUIDs are printed with a leading '@' character, and in quotes.
            // Expect a quote after the '@' in the plan.
            ASSERT_EQUALS('\"', str[atIndex + 1]);
            // The next character is a quote. Find the close quote.
            auto closeQuote = str.find('"', atIndex + 2);
            str = str.substr(0, atIndex + 2) + "<collUUID>" + str.substr(closeQuote, str.length());
        }

        return str;
    }

    void runNodeVariation(GoldenTestContext& gctx,
                          const std::string& name,
                          const ABT& n,
                          boost::optional<opt::unordered_map<std::string, IndexDefinition>>
                              collIndexDefs = boost::none) {
        auto& stream = gctx.outStream();
        if (stream.tellp()) {
            stream << std::endl;
        }
        stream << "==== VARIATION: " << name << " ====" << std::endl;
        stream << "-- INPUT:" << std::endl;
        stream << ExplainGenerator::explainV2(n) << std::endl;
        stream << "-- OUTPUT:" << std::endl;
        auto env = VariableEnvironment::build(n);
        SlotVarMap map;
        boost::optional<sbe::value::SlotId> ridSlot;
        sbe::value::SlotIdGenerator ids;
        opt::unordered_map<std::string, ScanDefinition> scanDefs;

        scanDefs.insert({"collName",
                         collIndexDefs.has_value() ? buildScanDefinition(collIndexDefs.value())
                                                   : buildScanDefinition()});
        scanDefs.insert({"otherColl", buildScanDefinition()});

        Metadata md(scanDefs);
        auto planStage = SBENodeLowering{env, map, ridSlot, ids, md, _nodeMap, false}.optimize(n);
        sbe::DebugPrinter printer;
        stream << stripUUIDs(printer.print(*planStage)) << std::endl;

        // After a variation is run, presumably any more variations in the test will use a new tree,
        // so reset the node map.
        _nodeMap = NodeToGroupPropsMap{};
        _fieldProjMap = {{}, {ProjectionName{scanLabel}}, {}};
        lastNodeGenerated = 0;
    }

    ScanDefinition buildScanDefinition(
        opt::unordered_map<std::string, IndexDefinition> indexDefs = {}) {
        ScanDefOptions opts;
        opts.insert({"type", "mongod"});
        opts.insert({"database", "test"});
        opts.insert({"uuid", UUID::gen().toString()});

        MultikeynessTrie trie;
        DistributionAndPaths dnp(DistributionType::Centralized);
        bool exists = true;
        CEType ce{false};
        return ScanDefinition(opts, indexDefs, trie, dnp, exists, ce);
    }

    // Does not add the node to the Node map, must be called inside '_node()'.
    ABT scanForTest(std::string coll = "collName") {
        return make<PhysicalScanNode>(_fieldProjMap, coll, false);
    }

    auto getNextNodeID() {
        return lastNodeGenerated++;
    }

    auto makeNodeProp() {
        NodeProps n{getNextNodeID(),
                    {},
                    {},
                    {},
                    boost::none,
                    CostType::fromDouble(0),
                    CostType::fromDouble(0),
                    {false}};
        properties::setPropertyOverwrite(n._physicalProps, properties::ProjectionRequirement({}));
        return n;
    }
    void runPathLowering(ABT& tree) {
        auto env = VariableEnvironment::build(tree);
        auto prefixId = PrefixId::createForTests();
        runPathLowering(env, prefixId, tree);
    }

    /**
     * Run passed in ABT through path lowering and return the same ABT. Useful for constructing
     * physical ABTs in-line for lowering tests.
     */
    ABT&& _path(ABT&& tree) {
        runPathLowering(tree);
        return std::move(tree);
    }

    /**
     * Register the passed-in ABT in the test's node map and return the same ABT. Useful for
     * constructing physical ABTs in-line for lowering tests.
     */
    ABT&& _node(ABT&& tree) {
        _nodeMap.insert({tree.cast<Node>(), makeNodeProp()});
        return std::move(tree);
    }

    ABT&& _node(ABT&& tree, NodeProps n) {
        _nodeMap.insert({tree.cast<Node>(), n});
        return std::move(tree);
    }

    void runPathLowering(VariableEnvironment& env, PrefixId& prefixId, ABT& tree) {
        // Run rewriters while things change
        bool changed = false;
        do {
            changed = false;
            if (PathLowering{prefixId, env}.optimize(tree)) {
                changed = true;
            }
            if (ConstEval{env}.optimize(tree)) {
                changed = true;
            }
        } while (changed);
    }

    ABT createBindings(std::vector<std::pair<std::string, std::string>> bindingList,
                       ABT source,
                       std::string sourceBinding) {
        for (auto [fieldName, bindingName] : bindingList) {
            auto field =
                make<EvalPath>(make<PathGet>(FieldNameType(fieldName), make<PathIdentity>()),
                               make<Variable>(ProjectionName(sourceBinding)));
            runPathLowering(field);
            ABT evalNode = make<EvaluationNode>(
                ProjectionName(bindingName), std::move(field), std::move(source));
            source = std::move(_node(std::move(evalNode)));
        }
        return source;
    }

    // Create bindings (as above) and also create a scan node source.
    ABT createBindings(std::vector<std::pair<std::string, std::string>> bindingList) {
        return createBindings(bindingList, _node(scanForTest()), "scan0");
    }

private:
    int32_t lastNodeGenerated = 0;
};


TEST_F(ABTPlanGeneration, LowerConstantExpression) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    runExpressionVariation(ctx, "string", Constant::str("hello world"_sd));

    runExpressionVariation(ctx, "int64", Constant::int64(100));
    runExpressionVariation(ctx, "int32", Constant::int32(32));
    runExpressionVariation(ctx, "double", Constant::fromDouble(3.14));
    runExpressionVariation(ctx, "decimal", Constant::fromDecimal(Decimal128("3.14")));

    runExpressionVariation(ctx, "timestamp", Constant::timestamp(Timestamp::max()));
    runExpressionVariation(ctx, "date", Constant::date(Date_t::fromMillisSinceEpoch(100)));

    runExpressionVariation(ctx, "boolean true", Constant::boolean(true));
    runExpressionVariation(ctx, "boolean false", Constant::boolean(false));
}

TEST_F(ABTPlanGeneration, LowerCollationNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    properties::PhysProps physProps;
    properties::setPropertyOverwrite<properties::ProjectionRequirement>(
        physProps, properties::ProjectionRequirement(ProjectionNameOrderPreservingSet({"sortA"})));
    NodeProps collationNodeProp{getNextNodeID(),
                                {},
                                {},
                                physProps,
                                boost::none,
                                CostType::fromDouble(0),
                                CostType::fromDouble(0),
                                {false}};

    runNodeVariation(ctx,
                     "Lower collation node with single field",
                     _node(make<CollationNode>(properties::CollationRequirement(
                                                   {{"sortA", CollationOp::Ascending}}),
                                               createBindings({{"a", "sortA"}})),
                           collationNodeProp));

    // Sort on multiple fields.
    properties::PhysProps physProps2;
    properties::setPropertyOverwrite<properties::ProjectionRequirement>(
        physProps2,
        properties::ProjectionRequirement(ProjectionNameOrderPreservingSet({"sortA", "sortB"})));
    NodeProps collationNodeProp2{getNextNodeID(),
                                 {},
                                 {},
                                 physProps2,
                                 boost::none,
                                 CostType::fromDouble(0),
                                 CostType::fromDouble(0),
                                 {false}};
    runNodeVariation(ctx,
                     "Lower collation node with two fields",
                     _node(make<CollationNode>(properties::CollationRequirement(
                                                   {{"sortA", CollationOp::Ascending},
                                                    {"sortB", CollationOp::Descending}}),
                                               createBindings({{"a", "sortA"}, {"b", "sortB"}})),
                           collationNodeProp2));
}

TEST_F(ABTPlanGeneration, LowerCoScanNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    runNodeVariation(ctx, "CoScan", _node(make<CoScanNode>()));
}

TEST_F(ABTPlanGeneration, LowerMultipleEvaluationNodes) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    runNodeVariation(ctx,
                     "Lower two chained evaluation nodes",
                     createBindings({{"a", "proj0"}, {"b", "proj1"}}));
}

TEST_F(ABTPlanGeneration, LowerFilterNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    runNodeVariation(
        ctx,
        "filter for: a >= 23",
        _node(make<FilterNode>(
            _path(make<EvalFilter>(
                make<PathGet>("a", make<PathCompare>(Operations::Gte, Constant::int32(23))),
                make<Variable>(scanLabel))),
            _node(scanForTest()))));

    runNodeVariation(
        ctx,
        "filter for constant: true",
        _node(make<FilterNode>(_path(make<EvalFilter>(make<PathConstant>(Constant::boolean(true)),
                                                      make<Variable>(scanLabel))),
                               _node(scanForTest()))));
}

TEST_F(ABTPlanGeneration, LowerGroupByNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    std::vector<GroupNodeType> groupTypes{
        GroupNodeType::Complete, GroupNodeType::Local, GroupNodeType::Global};

    for (const auto& groupType : groupTypes) {
        runNodeVariation(
            ctx,
            str::stream() << "GroupByNode one output with type "
                          << GroupNodeTypeEnum::toString[static_cast<int>(groupType)],
            _node(make<GroupByNode>(
                ProjectionNameVector{"key1", "key2"},
                ProjectionNameVector{"outFunc1"},
                makeSeq(make<FunctionCall>("$sum", makeSeq(make<Variable>("aggInput1")))),
                groupType,
                createBindings({{"a", "key1"}, {"b", "key2"}, {"c", "aggInput1"}}))));

        runNodeVariation(
            ctx,
            str::stream() << "GroupByNode multiple outputs with type "
                          << GroupNodeTypeEnum::toString[static_cast<int>(groupType)],
            _node(make<GroupByNode>(
                ProjectionNameVector{"key1", "key2"},
                ProjectionNameVector{"outFunc1", "outFunc2"},
                makeSeq(make<FunctionCall>("$sum", makeSeq(make<Variable>("aggInput1"))),
                        make<FunctionCall>("$sum", makeSeq(make<Variable>("aggInput2")))),
                groupType,
                createBindings(
                    {{"a", "key1"}, {"b", "key2"}, {"c", "aggInput1"}, {"d", "aggInput2"}}))));
    }
}

TEST_F(ABTPlanGeneration, LowerHashJoinNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    // Arguments may be evaluated in any order, and since _node() assigns incrementing stage IDs,
    // nodes with multiple children must have the children defined before the parent to ensure
    // deterministic ordering.
    auto child1 = _node(make<EvaluationNode>(
        "otherID",
        _path(make<EvalPath>(make<PathGet>("other_id", make<PathIdentity>()),
                             make<Variable>(ProjectionName{"scan0"}))),
        _node(make<PhysicalScanNode>(
            FieldProjectionMap{{}, {ProjectionName{"scan0"}}, {}}, "collName", false))));

    auto child2 = _node(make<EvaluationNode>(
        "ID",
        _path(make<EvalPath>(make<PathGet>("id", make<PathIdentity>()),
                             make<Variable>(ProjectionName{"scan1"}))),
        _node(make<PhysicalScanNode>(
            FieldProjectionMap{{}, {ProjectionName{"scan1"}}, {}}, "otherColl", false))));

    runNodeVariation(ctx,
                     "Hash join with one equality",
                     _node(make<HashJoinNode>(JoinType::Inner,
                                              std::vector<ProjectionName>{"otherID"},
                                              std::vector<ProjectionName>{"ID"},
                                              std::move(child1),
                                              std::move(child2))));

    child1 = createBindings(
        {{"city", "proj0"}, {"state", "proj1"}},
        _node(make<PhysicalScanNode>(
            FieldProjectionMap{{}, {ProjectionName{"scan0"}}, {}}, "collName", false)),
        "scan0");

    child2 = createBindings(
        {{"cityField", "proj2"}, {"state_id", "proj3"}},
        _node(make<PhysicalScanNode>(
            FieldProjectionMap{{}, {ProjectionName{"scan1"}}, {}}, "otherColl", false)),
        "scan1");

    runNodeVariation(ctx,
                     "Hash join with two equalities",
                     _node(make<HashJoinNode>(JoinType::Inner,
                                              std::vector<ProjectionName>{"proj0", "proj1"},
                                              std::vector<ProjectionName>{"proj2", "proj3"},
                                              std::move(child1),
                                              std::move(child2))));
}

TEST_F(ABTPlanGeneration, LowerIndexScanNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    // Generate for simple interval and compound interval
    opt::unordered_map<std::string, IndexDefinition> indexDefs = {
        {"index0", makeIndexDefinition("a", CollationOp::Ascending, false)}};

    for (int i = 0; i <= 1; i++) {
        bool isReversed = i == 1;
        auto reversedString = isReversed ? "reverse" : "forward";
        // Basic index scan with RID
        runNodeVariation(ctx,
                         str::stream() << "Basic " << reversedString << " index scan with RID",
                         _node(make<IndexScanNode>(
                             FieldProjectionMap{{ProjectionName{"rid"}}, {}, {}},
                             "collName",
                             "index0",
                             CompoundIntervalRequirement{IntervalRequirement(
                                 BoundRequirement(i > 0, Constant::fromDouble(23 + i * 4)),
                                 BoundRequirement(i == 0, Constant::fromDouble(35 + i * 100)))},
                             isReversed)),
                         indexDefs);


        // Covering index scan with one field
        runNodeVariation(
            ctx,
            str::stream() << "Covering " << reversedString << " index scan with one field",
            _node(make<IndexScanNode>(
                FieldProjectionMap{{}, {}, {{"<indexKey> 0", ProjectionName{"proj0"}}}},
                "collName",
                "index0",
                CompoundIntervalRequirement{IntervalRequirement(
                    BoundRequirement(i >= 0, Constant::fromDouble(23 + (i + 1) * 3)),
                    BoundRequirement(i > 0, Constant::fromDouble(35 + ((i * 3) * (i * 4)))))},
                isReversed)),
            indexDefs);
    }
}

TEST_F(ABTPlanGeneration, LowerLimitSkipNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    // Just Limit
    runNodeVariation(
        ctx,
        "Lower single limit without skip",
        _node(make<LimitSkipNode>(properties::LimitSkipRequirement(5, 0), _node(scanForTest()))));

    // Just Skip
    runNodeVariation(
        ctx,
        "Lower single skip without limit",
        _node(make<LimitSkipNode>(properties::LimitSkipRequirement(0, 4), _node(scanForTest()))));

    // Limit and Skip
    runNodeVariation(
        ctx,
        "Lower LimitSkip node with values for both limit and skip",
        _node(make<LimitSkipNode>(properties::LimitSkipRequirement(4, 2), _node(scanForTest()))));
}

TEST_F(ABTPlanGeneration, LowerMergeJoinNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    std::vector<CollationOp> ops = {CollationOp::Ascending, CollationOp::Descending};
    // Run a variation for each supported collation.
    for (auto& op : ops) {
        auto child1 = createBindings(
            {{"other_id", "proj0"}},
            _node(make<PhysicalScanNode>(
                FieldProjectionMap{{}, {ProjectionName{"scan0"}}, {}}, "collName", false)),
            "scan0");
        auto child2 = createBindings(
            {{"id", "proj1"}},
            _node(make<PhysicalScanNode>(
                FieldProjectionMap{{}, {ProjectionName{"scan1"}}, {}}, "otherColl", false)),
            "scan1");
        runNodeVariation(ctx,
                         str::stream() << "Lower merge join with one projection (collation="
                                       << CollationOpEnum::toString[static_cast<int>(op)] << ")",
                         _node(make<MergeJoinNode>(ProjectionNameVector{ProjectionName{"proj0"}},
                                                   ProjectionNameVector{ProjectionName{"proj1"}},
                                                   std::vector<CollationOp>{op},
                                                   std::move(child1),
                                                   std::move(child2))));
    }

    // Run variations with two projections and every possible combination of collation.
    for (auto& op1 : ops) {
        for (auto& op2 : ops) {
            auto child1 = createBindings(
                {{"other_id", "proj0"}, {"city", "proj2"}},
                _node(make<PhysicalScanNode>(
                    FieldProjectionMap{{}, {ProjectionName{"scan0"}}, {}}, "collName", false)),
                "scan0");
            auto child2 = createBindings(
                {{"id", "proj1"}, {"city", "proj3"}},
                _node(make<PhysicalScanNode>(
                    FieldProjectionMap{{}, {ProjectionName{"scan1"}}, {}}, "otherColl", false)),
                "scan1");

            runNodeVariation(
                ctx,
                str::stream() << "Lower merge join with two projections (collation="
                              << CollationOpEnum::toString[static_cast<int>(op1)] << ", "
                              << CollationOpEnum::toString[static_cast<int>(op2)] << ")",
                _node(make<MergeJoinNode>(
                    ProjectionNameVector{ProjectionName{"proj0"}, ProjectionName{"proj2"}},
                    ProjectionNameVector{ProjectionName{"proj1"}, ProjectionName{"proj3"}},
                    std::vector<CollationOp>{op1, op2},
                    std::move(child1),
                    std::move(child2))));
        }
    }
}

TEST_F(ABTPlanGeneration, LowerNestedLoopJoinNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    // Run a variation for both supported join types.
    std::vector<JoinType> joins = {JoinType::Inner, JoinType::Left};
    for (auto& joinType : joins) {
        auto child1 = createBindings(
            {{"city", "proj0"}},
            _node(make<PhysicalScanNode>(
                FieldProjectionMap{{}, {ProjectionName{"scan0"}}, {}}, "collName", false)),
            "scan0");
        auto child2 = createBindings(
            {{"id", "proj1"}},
            _node(make<PhysicalScanNode>(
                FieldProjectionMap{{}, {ProjectionName{"scan1"}}, {}}, "otherColl", false)),
            "scan1");

        runNodeVariation(
            ctx,
            str::stream() << "Nested loop join with equality predicate ("
                          << JoinTypeEnum::toString[static_cast<int>(joinType)] << " join)",
            _node(make<NestedLoopJoinNode>(
                joinType,
                ProjectionNameSet{"proj0"},
                _path(make<EvalFilter>(
                    make<PathCompare>(Operations::Eq, make<Variable>(ProjectionName{"proj1"})),
                    make<Variable>(ProjectionName{"proj0"}))),
                std::move(child1),
                std::move(child2))));
    }
}

TEST_F(ABTPlanGeneration, LowerPhysicalScanNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    for (auto i = 0; i <= 1; i++) {
        auto isParallel = i == 1;
        auto parallelString = isParallel ? "(parallel)" : "(not parallel)";
        runNodeVariation(
            ctx,
            str::stream() << "Physical scan with root projection " << parallelString,
            _node(make<PhysicalScanNode>(
                FieldProjectionMap{{}, {ProjectionName{"root0"}}, {}}, "collName", isParallel)));

        runNodeVariation(
            ctx,
            str::stream() << "Physical scan with RID projection " << parallelString,
            _node(make<PhysicalScanNode>(
                FieldProjectionMap{{ProjectionName{"RID0"}}, {}, {}}, "collName", isParallel)));

        runNodeVariation(
            ctx,
            str::stream() << "Physical scan with root and RID projections " << parallelString,
            _node(make<PhysicalScanNode>(
                FieldProjectionMap{{ProjectionName{"RID0"}}, {ProjectionName{"root0"}}, {}},
                "collName",
                isParallel)));

        runNodeVariation(
            ctx,
            str::stream() << "Physical scan with root, RID and field projections "
                          << parallelString,
            _node(make<PhysicalScanNode>(
                FieldProjectionMap{{ProjectionName{"RID0"}},
                                   {ProjectionName{"root0"}},
                                   {{FieldNameType{"field"}, {ProjectionName{"field2"}}}}},
                "collName",
                isParallel)));
    }
}

TEST_F(ABTPlanGeneration, LowerSeekNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    auto indexScan =
        _node(make<IndexScanNode>(FieldProjectionMap{{ProjectionName{"rid"}}, {}, {}},
                                  "collName",
                                  "index0",
                                  CompoundIntervalRequirement{IntervalRequirement(
                                      BoundRequirement(false, Constant::fromDouble(23)),
                                      BoundRequirement(true, Constant::fromDouble(35)))},
                                  false));

    auto seek = _node(make<LimitSkipNode>(
        properties::LimitSkipRequirement(1, 0),
        _node(make<SeekNode>(ProjectionName{"rid"}, _fieldProjMap, "collName"))));

    opt::unordered_map<std::string, IndexDefinition> indexDefs = {
        {"index0", makeIndexDefinition("a", CollationOp::Ascending, false)}};

    runNodeVariation(ctx,
                     "index seek",
                     _node(make<NestedLoopJoinNode>(JoinType::Inner,
                                                    ProjectionNameSet{"rid"},
                                                    Constant::boolean(true),
                                                    std::move(indexScan),
                                                    std::move(seek))),
                     indexDefs);
}
TEST_F(ABTPlanGeneration, LowerSortedMergeNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    std::vector<CollationOp> ops = {CollationOp::Ascending, CollationOp::Descending};
    auto runVariations = [&](auto req, auto op, auto& suffix) {
        runNodeVariation(ctx,
                         str::stream() << "one source " << suffix,
                         _node(make<SortedMergeNode>(
                             req, makeSeq(createBindings({{"a", "proj0"}, {"b", "proj1"}})))));

        auto left = createBindings({{"a", "proj0"}, {"b", "proj1"}});
        auto right = createBindings({{"a", "proj0"}, {"b", "proj1"}});
        runNodeVariation(
            ctx,
            str::stream() << "two sources " << suffix,
            _node(make<SortedMergeNode>(req, makeSeq(std::move(left), std::move(right)))));


        auto child1 = createBindings({{"a", "proj0"}, {"b", "proj1"}});
        auto child2 = createBindings({{"a", "proj0"}, {"b", "proj1"}});
        auto child3 = createBindings({{"a", "proj0"}, {"b", "proj1"}});
        auto child4 = createBindings({{"a", "proj0"}, {"b", "proj1"}});
        auto child5 = createBindings({{"a", "proj0"}, {"b", "proj1"}});
        runNodeVariation(ctx,
                         str::stream() << "five sources " << suffix,
                         _node(make<SortedMergeNode>(req,
                                                     makeSeq(std::move(child1),
                                                             std::move(child2),
                                                             std::move(child3),
                                                             std::move(child4),
                                                             std::move(child5)))));
    };
    for (auto& op : ops) {
        runVariations(properties::CollationRequirement(
                          ProjectionCollationSpec({{ProjectionName{"proj0"}, op}})),
                      op,
                      str::stream()
                          << "sorted on `a` " << CollationOpEnum::toString[static_cast<int>(op)]);
        for (auto& op2 : ops) {
            runVariations(properties::CollationRequirement(ProjectionCollationSpec(
                              {{ProjectionName{"proj0"}, op}, {ProjectionName{"proj1"}, op2}})),
                          op,
                          str::stream()
                              << "sorted on `a` " << CollationOpEnum::toString[static_cast<int>(op)]
                              << " and `b` " << CollationOpEnum::toString[static_cast<int>(op2)]);
        }
    }
}

TEST_F(ABTPlanGeneration, LowerSpoolNodes) {
    // This test tests both SpoolProducerNode and SpoolConsumerNode.
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    std::vector<SpoolProducerType> spoolPTypes = {SpoolProducerType::Eager,
                                                  SpoolProducerType::Lazy};
    std::vector<SpoolConsumerType> spoolCTypes = {SpoolConsumerType::Regular,
                                                  SpoolConsumerType::Stack};
    for (const auto& spoolProdType : spoolPTypes) {
        for (const auto& spoolConsumeType : spoolCTypes) {
            auto leftTree = _node(make<SpoolProducerNode>(spoolProdType,
                                                          1,
                                                          ProjectionNameVector{"proj0"},
                                                          Constant::boolean(true),
                                                          createBindings({{"a", "proj0"}})));
            auto rightTree =
                _node(make<SpoolConsumerNode>(spoolConsumeType, 1, ProjectionNameVector{"proj0"}));
            runNodeVariation(
                ctx,
                str::stream() << "Spool in union with "
                              << SpoolProducerTypeEnum::toString[static_cast<int>(spoolProdType)]
                              << " producer and "
                              << SpoolConsumerTypeEnum::toString[static_cast<int>(spoolConsumeType)]
                              << " consumer",
                _node(make<UnionNode>(ProjectionNameVector{"proj0"},
                                      makeSeq(std::move(leftTree), std::move(rightTree)))));
        }
    }

    // Test with a more interesting filter.
    auto filterTree = _path(make<EvalFilter>(
        make<PathGet>("b", make<PathCompare>(Operations::Gte, Constant::int32(23))),
        make<Variable>("scan0")));
    auto leftTree = _node(make<SpoolProducerNode>(SpoolProducerType::Lazy,
                                                  1,
                                                  ProjectionNameVector{"proj0"},
                                                  std::move(filterTree),
                                                  createBindings({{"a", "proj0"}})));
    auto rightTree =
        _node(make<SpoolConsumerNode>(SpoolConsumerType::Stack, 1, ProjectionNameVector{"proj0"}));
    runNodeVariation(ctx,
                     "Spool in union with filter expression",
                     _node(make<UnionNode>(ProjectionNameVector{"proj0"},
                                           makeSeq(std::move(leftTree), std::move(rightTree)))));
}

TEST_F(ABTPlanGeneration, LowerUnionNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    // Test a union with only one child.
    auto leftTree = createBindings({{"a", "proj0"}, {"b", "proj1"}});
    runNodeVariation(
        ctx,
        "UnionNode with only one child",
        _node(make<UnionNode>(ProjectionNameVector{"proj0"}, makeSeq(std::move(leftTree)))));

    // Test a union with two children.
    leftTree = createBindings({{"a", "proj0"}, {"b", "left1"}});
    auto rightTree = createBindings({{"a", "proj0"}, {"b", "right1"}});
    runNodeVariation(ctx,
                     "UnionNode with two children",
                     _node(make<UnionNode>(ProjectionNameVector{"proj0"},
                                           makeSeq(std::move(leftTree), std::move(rightTree)))));

    // Test a union with many children.
    auto aTree = createBindings({{"a", "proj0"}, {"b", "a1"}});
    auto bTree = createBindings({{"a", "proj0"}, {"b", "b1"}});
    auto cTree = createBindings({{"a", "proj0"}, {"b", "c1"}});
    auto dTree = createBindings({{"a", "proj0"}, {"b", "d1"}});
    auto eTree = createBindings({{"a", "proj0"}, {"b", "e1"}});
    runNodeVariation(ctx,
                     "UnionNode with many children",
                     _node(make<UnionNode>(ProjectionNameVector{"proj0"},
                                           makeSeq(std::move(aTree),
                                                   std::move(bTree),
                                                   std::move(cTree),
                                                   std::move(dTree),
                                                   std::move(eTree)))));
}

TEST_F(ABTPlanGeneration, LowerUniqueNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    runNodeVariation(
        ctx,
        "Lower unique node with single key",
        _node(make<UniqueNode>(ProjectionNameVector{"proj0"}, createBindings({{"a", "proj0"}}))));


    runNodeVariation(
        ctx,
        "Lower unique node with multiple keys",
        _node(make<UniqueNode>(ProjectionNameVector{"proj0", "proj1", "proj2"},
                               createBindings({{"a", "proj0"}, {"b", "proj1"}, {"c", "proj2"}}))));
}

TEST_F(ABTPlanGeneration, LowerUnwindNode) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);
    runNodeVariation(ctx,
                     "Lower UnwindNode discard non-arrays",
                     _node(make<UnwindNode>(ProjectionName("proj0"),
                                            ProjectionName("proj0_pid"),
                                            false,
                                            createBindings({{"a", "proj0"}}))));

    runNodeVariation(ctx,
                     "Lower UnwindNode keep non-arrays",
                     _node(make<UnwindNode>(ProjectionName("proj0"),
                                            ProjectionName("proj0_pid"),
                                            true,
                                            createBindings({{"a", "proj0"}}))));
}

TEST_F(ABTPlanGeneration, LowerVarExpression) {
    GoldenTestContext ctx(&goldenTestConfig);
    ctx.printTestHeader(GoldenTestContext::HeaderFormat::Text);

    runNodeVariation(
        ctx,
        "varInProj",
        _node(make<EvaluationNode>("proj0",
                                   _path(make<EvalPath>(make<PathGet>("a", make<PathIdentity>()),
                                                        make<Variable>(scanLabel))),
                                   _node(scanForTest()))));
}

}  // namespace
}  // namespace mongo::optimizer
