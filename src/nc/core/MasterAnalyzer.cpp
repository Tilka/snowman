/* The file is part of Snowman decompiler.             */
/* See doc/licenses.txt for the licensing information. */

//
// SmartDec decompiler - SmartDec is a native code to C/C++ decompiler
// Copyright (C) 2015 Alexander Chernov, Katerina Troshina, Yegor Derevenets,
// Alexander Fokin, Sergey Levin, Leonid Tsvetkov
//
// This file is part of SmartDec decompiler.
//
// SmartDec decompiler is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// SmartDec decompiler is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with SmartDec decompiler.  If not, see <http://www.gnu.org/licenses/>.
//

#include "MasterAnalyzer.h"

#include <QObject> /* For QObject::tr() */

#include <cstdint> /* uintptr_t */

#include <nc/common/Foreach.h>
#include <nc/common/make_unique.h>

#include <nc/core/Context.h>
#include <nc/core/Module.h>
#include <nc/core/arch/irgen/IRGenerator.h>
#include <nc/core/ir/BasicBlock.h>
#include <nc/core/ir/Function.h>
#include <nc/core/ir/Functions.h>
#include <nc/core/ir/FunctionsGenerator.h>
#include <nc/core/ir/Program.h>
#include <nc/core/ir/calling/Conventions.h>
#include <nc/core/ir/calling/Hooks.h>
#include <nc/core/ir/calling/SignatureAnalyzer.h>
#include <nc/core/ir/calling/Signatures.h>
#include <nc/core/ir/cflow/Graphs.h>
#include <nc/core/ir/cflow/GraphBuilder.h>
#include <nc/core/ir/cflow/StructureAnalyzer.h>
#include <nc/core/ir/cgen/CodeGenerator.h>
#include <nc/core/ir/dflow/Dataflows.h>
#include <nc/core/ir/dflow/DataflowAnalyzer.h>
#include <nc/core/ir/liveness/Livenesses.h>
#include <nc/core/ir/liveness/LivenessAnalyzer.h>
#include <nc/core/ir/misc/TermToFunction.h>
#include <nc/core/ir/types/TypeAnalyzer.h>
#include <nc/core/ir/types/Types.h>
#include <nc/core/ir/vars/VariableAnalyzer.h>
#include <nc/core/ir/vars/Variables.h>
#include <nc/core/likec/Tree.h>
#include <nc/core/mangling/Demangler.h>

#ifdef NC_TREE_CHECKS
#include <nc/core/ir/misc/CensusVisitor.h>
#include <nc/core/likec/Expression.h>
#include <nc/core/likec/Statement.h>
#endif

namespace nc {
namespace core {

MasterAnalyzer::~MasterAnalyzer() {}

void MasterAnalyzer::createProgram(Context &context) const {
    context.logToken() << tr("Creating intermediate representation of the program.");

    std::unique_ptr<ir::Program> program(new ir::Program());

    core::arch::irgen::IRGenerator(context.module().get(), context.instructions().get(), program.get())
        .generate(context.cancellationToken());

    context.setProgram(std::move(program));
}

void MasterAnalyzer::createFunctions(Context &context) const {
    context.logToken() << tr("Creating functions.");

    std::unique_ptr<ir::Functions> functions(new ir::Functions);

    ir::FunctionsGenerator().makeFunctions(*context.program(), *functions);

    foreach (ir::Function *function, functions->list()) {
        pickFunctionName(context, function);
    }

    context.setFunctions(std::move(functions));
}

void MasterAnalyzer::pickFunctionName(Context &context, ir::Function *function) const {
    /* If the function has an entry, and the entry has an address... */
    if (function->entry() && function->entry()->address()) {
        QString name = context.module()->getName(*function->entry()->address());

        if (!name.isEmpty()) {
            /* Take the name of the corresponding symbol, if possible. */
            QString cleanName = likec::Tree::cleanName(name);
            function->setName(cleanName);

            if (name != cleanName) {
                function->comment().append(name);
            }

            QString demangledName = context.module()->demangler()->demangle(name);
            if (demangledName.contains('(')) {
                /* What we demangled has really something to do with a function. */
                function->comment().append(demangledName);
            }
        } else {
            /* Invent a name based on the entry address. */
            function->setName(QString("func_%1").arg(*function->entry()->address(), 0, 16));
        }
    } else {
        /* If there are no other options, invent some random unique name. */
        function->setName(QString("func_noentry_%1").arg(reinterpret_cast<std::uintptr_t>(function), 0, 16));
    }
}

void MasterAnalyzer::detectCallingConvention(Context & /*context*/, const ir::calling::CalleeId &/*descriptor*/) const {
    /* Nothing to do. */
}

void MasterAnalyzer::dataflowAnalysis(Context &context) const {
    context.logToken() << tr("Dataflow analysis.");

    if (!context.signatures()) {
        context.setSignatures(std::make_unique<ir::calling::Signatures>());
    }
    if (!context.conventions()) {
        context.setConventions(std::make_unique<ir::calling::Conventions>());
    }

    context.setHooks(std::make_unique<ir::calling::Hooks>(*context.conventions(), *context.signatures()));
    context.hooks()->setConventionDetector([this, &context](const ir::calling::CalleeId &calleeId) {
        this->detectCallingConvention(context, calleeId);
    });

    context.setDataflows(std::make_unique<ir::dflow::Dataflows>());

    foreach (auto function, context.functions()->list()) {
        dataflowAnalysis(context, function);
        context.cancellationToken().poll();
    }
}

void MasterAnalyzer::dataflowAnalysis(Context &context, const ir::Function *function) const {
    context.logToken() << tr("Dataflow analysis of %1.").arg(function->name());

    std::unique_ptr<ir::dflow::Dataflow> dataflow(new ir::dflow::Dataflow());

    ir::dflow::DataflowAnalyzer(*dataflow, context.module()->architecture(), function, context.hooks())
        .analyze(context.cancellationToken());

    context.dataflows()->emplace(function, std::move(dataflow));
}

void MasterAnalyzer::reconstructSignatures(Context &context) const {
    context.logToken() << tr("Reconstructing function signatures.");

    auto signatures = std::make_unique<ir::calling::Signatures>();

    ir::calling::SignatureAnalyzer(*signatures, *context.dataflows(), *context.hooks())
        .analyze(context.cancellationToken());

    context.setSignatures(std::move(signatures));
}

void MasterAnalyzer::reconstructVariables(Context &context) const {
    context.logToken() << tr("Reconstructing variables.");

    std::unique_ptr<ir::vars::Variables> variables(new ir::vars::Variables());

    ir::vars::VariableAnalyzer(*variables, *context.dataflows(), context.module()->architecture()).analyze();

    context.setVariables(std::move(variables));
}

void MasterAnalyzer::livenessAnalysis(Context &context) const {
    context.logToken() << tr("Liveness analysis.");

    context.setLivenesses(std::make_unique<ir::liveness::Livenesses>());

    foreach (const ir::Function *function, context.functions()->list()) {
        livenessAnalysis(context, function);
    }
}

void MasterAnalyzer::livenessAnalysis(Context &context, const ir::Function *function) const {
    context.logToken() << tr("Liveness analysis of %1.").arg(function->name());

    std::unique_ptr<ir::liveness::Liveness> liveness(new ir::liveness::Liveness());

    ir::liveness::LivenessAnalyzer(*liveness, function,
        *context.dataflows()->at(function), context.module()->architecture(),
        *context.graphs()->at(function), *context.hooks(), *context.signatures())
    .analyze();

    context.livenesses()->emplace(function, std::move(liveness));
}

void MasterAnalyzer::reconstructTypes(Context &context) const {
    context.logToken() << tr("Reconstructing types.");

    std::unique_ptr<ir::types::Types> types(new ir::types::Types());

    ir::types::TypeAnalyzer(
        *types, *context.functions(), *context.dataflows(), *context.variables(),
        *context.livenesses(), *context.hooks(), *context.signatures())
    .analyze(context.cancellationToken());

    context.setTypes(std::move(types));
}

void MasterAnalyzer::structuralAnalysis(Context &context) const {
    context.logToken() << tr("Structural analysis.");

    context.setGraphs(std::make_unique<ir::cflow::Graphs>());

    foreach (auto function, context.functions()->list()) {
        structuralAnalysis(context, function);
        context.cancellationToken().poll();
    }
}

void MasterAnalyzer::structuralAnalysis(Context &context, const ir::Function *function) const {
    context.logToken() << tr("Structural analysis of %1.").arg(function->name());

    std::unique_ptr<ir::cflow::Graph> graph(new ir::cflow::Graph());

    ir::cflow::GraphBuilder()(*graph, function);
    ir::cflow::StructureAnalyzer(*graph, *context.dataflows()->at(function)).analyze();

    context.graphs()->emplace(function, std::move(graph));
}

void MasterAnalyzer::generateTree(Context &context) const {
    context.logToken() << tr("Generating AST.");

    std::unique_ptr<nc::core::likec::Tree> tree(new nc::core::likec::Tree());

    ir::cgen::CodeGenerator(*tree, *context.module(), *context.functions(), *context.hooks(),
        *context.signatures(), *context.dataflows(), *context.variables(), *context.graphs(),
        *context.livenesses(), *context.types(), context.cancellationToken())
        .makeCompilationUnit();

    context.setTree(std::move(tree));
}

#ifdef NC_TREE_CHECKS
void MasterAnalyzer::checkTree(Context &context) const {
    context.logToken() << tr("Checking AST.");

    class TreeVisitor: public Visitor<likec::TreeNode> {
        boost::unordered_set<const ir::Statement *> statements_;
        boost::unordered_set<const ir::Term *> terms_;

        public:

        TreeVisitor(const ir::misc::CensusVisitor &visitor):
            statements_(visitor.statements().begin(), visitor.statements().end()),
            terms_(visitor.terms().begin(), visitor.terms().end())
        {}

        virtual void operator()(likec::TreeNode *node) override {
            if (const likec::Statement *statement = node->as<likec::Statement>()) {
                if (statement->statement()) {
                    assert(contains(statements_, statement->statement()));
                }
            } else if (const likec::Expression *expression = node->as<likec::Expression>()) {
                if (expression->term()) {
                    assert(contains(terms_, expression->term()));
                }

                auto type = expression->getType();
                assert(type != NULL);
#if 0
                // TODO: make decompiler not to emit warnings here
                if (type == expression->tree().makeErroneousType()) {
                    ncWarning(typeid(*node).name());
                }
#endif
            }
            node->visitChildNodes(*this);
        }
    };

    ir::misc::CensusVisitor visitor(context.hooks());
    foreach (const ir::Function *function, context.functions()->list()) {
        visitor(function);
    }

    TreeVisitor checker(visitor);
    checker(context.tree()->root());
}
#endif

void MasterAnalyzer::computeTermToFunctionMapping(Context &context) const {
    context.logToken() << tr("Computing term to function mapping.");

    context.setTermToFunction(std::unique_ptr<ir::misc::TermToFunction>(
        new ir::misc::TermToFunction(context.functions(), context.hooks())));
}

void MasterAnalyzer::decompile(Context &context) const {
    context.logToken() << tr("Decompiling.");

    createProgram(context);
    context.cancellationToken().poll();

    createFunctions(context);
    context.cancellationToken().poll();

    dataflowAnalysis(context);
    context.cancellationToken().poll();

    reconstructSignatures(context);
    context.cancellationToken().poll();

    dataflowAnalysis(context);
    context.cancellationToken().poll();

    reconstructVariables(context);
    context.cancellationToken().poll();

    structuralAnalysis(context);
    context.cancellationToken().poll();

    livenessAnalysis(context);
    context.cancellationToken().poll();

    reconstructTypes(context);
    context.cancellationToken().poll();

    generateTree(context);
    context.cancellationToken().poll();

#ifdef NC_TREE_CHECKS
    checkTree(context);
    context.cancellationToken().poll();
#endif

    computeTermToFunctionMapping(context);
    context.cancellationToken().poll();

    context.logToken() << tr("Decompilation completed.");
}

} // namespace core
} // namespace nc

/* vim:set et sts=4 sw=4: */
