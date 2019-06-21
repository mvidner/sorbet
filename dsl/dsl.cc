#include "dsl/dsl.h"
#include "ast/Helpers.h"
#include "ast/treemap/treemap.h"
#include "ast/verifier/verifier.h"
#include "common/typecase.h"
#include "dsl/ChalkODMProp.h"
#include "dsl/ClassNew.h"
#include "dsl/Command.h"
#include "dsl/DSLBuilder.h"
#include "dsl/Delegate.h"
#include "dsl/InterfaceWrapper.h"
#include "dsl/Minitest.h"
#include "dsl/MixinEncryptedProp.h"
#include "dsl/OpusEnum.h"
#include "dsl/Private.h"
#include "dsl/ProtobufDescriptorPool.h"
#include "dsl/Rails.h"
#include "dsl/Struct.h"
#include "dsl/attr_reader.h"

using namespace std;

namespace sorbet::dsl {

class DSLReplacer {
    friend class DSL;

public:
    unique_ptr<ast::ClassDef> postTransformClassDef(core::MutableContext ctx, unique_ptr<ast::ClassDef> classDef) {
        Command::patchDSL(ctx, classDef.get());
        Rails::patchDSL(ctx, classDef.get());
        OpusEnum::patchDSL(ctx, classDef.get());

        ast::Expression *prevStat = nullptr;
        UnorderedMap<ast::Expression *, vector<unique_ptr<ast::Expression>>> replaceNodes;
        vector<ChalkODMProp::Prop> props;
        for (auto &stat : classDef->rhs) {
            typecase(
                stat.get(),
                [&](ast::Assign *assign) {
                    auto nodes = Struct::replaceDSL(ctx, assign);
                    if (!nodes.empty()) {
                        replaceNodes[stat.get()] = std::move(nodes);
                        return;
                    }

                    nodes = ClassNew::replaceDSL(ctx, assign);
                    if (!nodes.empty()) {
                        replaceNodes[stat.get()] = std::move(nodes);
                        return;
                    }

                    nodes = ProtobufDescriptorPool::replaceDSL(ctx, assign);
                    if (!nodes.empty()) {
                        replaceNodes[stat.get()] = std::move(nodes);
                        return;
                    }
                },

                [&](ast::Send *send) {
                    // This one is different: it returns nodes and a prop.
                    auto nodesAndProp = ChalkODMProp::replaceDSL(ctx, send);
                    if (nodesAndProp.has_value()) {
                        ENFORCE(!nodesAndProp->nodes.empty(), "nodesAndProp with value must not have nodes be empty");
                        replaceNodes[stat.get()] = std::move(nodesAndProp->nodes);
                        props.push_back(std::move(nodesAndProp->prop));
                        return;
                    }

                    auto nodes = MixinEncryptedProp::replaceDSL(ctx, send);
                    if (!nodes.empty()) {
                        replaceNodes[stat.get()] = std::move(nodes);
                        return;
                    }

                    nodes = Minitest::replaceDSL(ctx, send);
                    if (!nodes.empty()) {
                        replaceNodes[stat.get()] = move(nodes);
                        return;
                    }

                    nodes = DSLBuilder::replaceDSL(ctx, send);
                    if (!nodes.empty()) {
                        replaceNodes[stat.get()] = std::move(nodes);
                        return;
                    }

                    nodes = Private::replaceDSL(ctx, send);
                    if (!nodes.empty()) {
                        replaceNodes[stat.get()] = std::move(nodes);
                        return;
                    }

                    nodes = Delegate::replaceDSL(ctx, send);
                    if (!nodes.empty()) {
                        replaceNodes[stat.get()] = std::move(nodes);
                        return;
                    }

                    // This one is different: it gets an extra prevStat argument.
                    nodes = AttrReader::replaceDSL(ctx, send, prevStat);
                    if (!nodes.empty()) {
                        replaceNodes[stat.get()] = std::move(nodes);
                        return;
                    }
                },

                [&](ast::Expression *e) {});

            prevStat = stat.get();
        }
        if (replaceNodes.empty()) {
            return classDef;
        }

        auto synthesizeInitialize = false;
        for (auto &a : classDef->ancestors) {
            if (ChalkODMProp::isTStruct(a.get())) {
                synthesizeInitialize = true;
                break;
            }
        }
        auto oldRHS = std::move(classDef->rhs);
        classDef->rhs.clear();
        // the `2` is: 1 for the sig, 1 for the method
        classDef->rhs.reserve(oldRHS.size() + (synthesizeInitialize ? 0 : 2));

        if (synthesizeInitialize) {
            // we define ours synthesized initialize first so that if the user wrote one themselves, it overrides ours.
            auto loc = classDef->loc;
            ast::MethodDef::ARGS_store args;
            ast::Hash::ENTRY_store sigKeys;
            ast::Hash::ENTRY_store sigVals;
            args.reserve(props.size());
            sigKeys.reserve(props.size());
            sigVals.reserve(props.size());
            for (auto &prop : props) {
                auto arg = ast::MK::KeywordArg(loc, ast::MK::Local(loc, prop.name));
                if (prop.optional) {
                    arg = ast::MK::OptionalArg(loc, std::move(arg), ast::MK::Unsafe(loc, ast::MK::Nil(loc)));
                }
                args.emplace_back(std::move(arg));
                sigKeys.emplace_back(ast::MK::Symbol(loc, prop.name));
                sigVals.emplace_back(std::move(prop.type));
            }
            classDef->rhs.emplace_back(
                ast::MK::SigVoid(loc, ast::MK::Hash(loc, std::move(sigKeys), std::move(sigVals))));
            classDef->rhs.emplace_back(ast::MK::Method(loc, loc, core::Names::initialize(), std::move(args),
                                                       ast::MK::EmptyTree(), ast::MethodDef::DSLSynthesized));
        }
        for (auto &stat : oldRHS) {
            if (replaceNodes.find(stat.get()) == replaceNodes.end()) {
                classDef->rhs.emplace_back(std::move(stat));
            } else {
                for (auto &newNode : replaceNodes.at(stat.get())) {
                    classDef->rhs.emplace_back(std::move(newNode));
                }
            }
        }
        return classDef;
    }

    unique_ptr<ast::Expression> postTransformSend(core::MutableContext ctx, unique_ptr<ast::Send> send) {
        return InterfaceWrapper::replaceDSL(ctx, std::move(send));
    }

private:
    DSLReplacer() = default;
};

unique_ptr<ast::Expression> DSL::run(core::MutableContext ctx, unique_ptr<ast::Expression> tree) {
    auto ast = std::move(tree);

    DSLReplacer dslReplacer;
    ast = ast::TreeMap::apply(ctx, dslReplacer, std::move(ast));
    auto verifiedResult = ast::Verifier::run(ctx, std::move(ast));
    return verifiedResult;
}

}; // namespace sorbet::dsl
