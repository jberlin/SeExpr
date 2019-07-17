/*
 Copyright Disney Enterprises, Inc.  All rights reserved.

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License
 and the following modification to it: Section 6 Trademarks.
 deleted and replaced with:

 6. Trademarks. This License does not grant permission to use the
 trade names, trademarks, service marks, or product names of the
 Licensor and its affiliates, except as required for reproducing
 the content of the NOTICE file.

 You may obtain a copy of the License at
 http://www.apache.org/licenses/LICENSE-2.0
*/
#ifndef MAKEDEPEND
#include <iostream>
#include <math.h>
#include <stack>
#include <algorithm>
#include <sstream>
#endif

#include "ExprConfig.h"
#include "ExprNode.h"
#include "ExprParseAction.h"
#include "ExprFunc.h"
#include "Expression.h"
#include "ExprType.h"
#include "ExprEnv.h"
#include "Platform.h"

#include "LLVMEvaluator.h"
#include "ExprWalker.h"

#include <cstdio>
#include <typeinfo>

namespace SeExpr2 {

// Get debugging flag from environment
bool Expression::debugging = getenv("SE_EXPR_DEBUG") != 0;
// Choose the defeault strategy based on what we've compiled with (SEEXPR_ENABLE_LLVM)
// And the environment variables SE_EXPR_DEBUG
static Expression::EvaluationStrategy chooseDefaultEvaluationStrategy()
{
    if (Expression::debugging) {
        std::cerr << "SeExpr2 Debug Mode Enabled " <<
#if defined(WINDOWS)
            _MSC_FULL_VER
#else
            __VERSION__
#endif
            << std::endl;
    }
#ifdef SEEXPR_ENABLE_LLVM
    if (char* env = getenv("SE_EXPR_EVAL")) {
        if (Expression::debugging)
            std::cerr << "Overriding SeExpr Evaluation Default to be " << env << std::endl;
        return !strcmp(env, "LLVM") ? Expression::UseLLVM : !strcmp(env, "INTERPRETER") ? Expression::UseInterpreter
                                                                                        : Expression::UseInterpreter;
    } else
        return Expression::UseLLVM;
#else
    return Expression::UseInterpreter;
#endif
}
Expression::EvaluationStrategy Expression::defaultEvaluationStrategy = chooseDefaultEvaluationStrategy();

class TypePrintExaminer : public SeExpr2::Examiner<true> {
  public:
    virtual bool examine(const SeExpr2::ExprNode* examinee);
    virtual void reset(){};
};

bool TypePrintExaminer::examine(const ExprNode* examinee)
{
    const ExprNode* curr = examinee;
    int depth = 0;
    char buf[1024];
    while (curr != 0) {
        depth++;
        curr = curr->parent();
    }
    sprintf(buf, "%*s", depth * 2, " ");
    std::cout << buf << "'" << examinee->toString() << "' " << typeid(*examinee).name()
              << " type=" << examinee->type().toString() << std::endl;

    return true;
};

class NullEvaluator : public Evaluator {
  public:
    virtual ~NullEvaluator()
    {
    }

    virtual void setDebugging(bool) override
    { /* do nothing */
    }
    virtual void dump() const override
    {
    }

    virtual bool prep(ExprNode*, ExprType)
    {
        return false;
    }
    virtual bool isValid() const override
    {
        return false;
    }

    virtual void evalFP(double*, VarBlock*) const override
    {
    }
    virtual void evalStr(char*, VarBlock*) const override
    {
    }
    virtual void evalMultiple(VarBlock*, double*, size_t, size_t) const override
    {
    }
};

Expression::Expression(Expression::EvaluationStrategy evaluationStrategyHint)
    : _wantVec(true)
    , _expression("")
    , _evaluationStrategyHint(evaluationStrategyHint)
    , _context(&Context::global())
    , _desiredReturnType(ExprType().FP(3).Varying())
    , _parseTree(nullptr)
    , _evaluator(nullptr)
    , _varBlockCreator(nullptr)
{
    ExprFunc::init();
}

Expression::Expression(const std::string& e,
                       const ExprType& type,
                       EvaluationStrategy evaluationStrategyHint,
                       const Context& context)
    : _wantVec(true)
    , _expression(e)
    , _evaluationStrategyHint(evaluationStrategyHint)
    , _context(&context)
    , _desiredReturnType(type)
    , _parseTree(nullptr)
    , _evaluator(nullptr)
    , _varBlockCreator(nullptr)
{
    ExprFunc::init();
}

Expression::~Expression()
{
    reset();
}

void Expression::reset()
{
    std::lock_guard<std::mutex> guard(_prepMutex);
    delete _evaluator;
    _evaluator = nullptr;
    delete _parseTree;
    _parseTree = nullptr;
    _parseError = "";
    _vars.clear();
    _funcs.clear();
    //_localVars.clear();
    _errors.clear();
    _threadUnsafeFunctionCalls.clear();
    _comments.clear();
    _results.resize(_desiredReturnType.dim());
}

void Expression::setContext(const Context& context)
{
    reset();
    _context = &context;
}

void Expression::setDesiredReturnType(const ExprType& type)
{
    if (_desiredReturnType != type) {
        reset();
        _desiredReturnType = type;
    }
}

void Expression::setVarBlockCreator(const VarBlockCreator* creator)
{
    if (_varBlockCreator != creator) {
        reset();
        _varBlockCreator = creator;
    }
}

void Expression::setExpr(const std::string& e)
{
    if (_expression != e) {
        reset();
        _expression = e;
    }
}

bool Expression::isConstant() const
{
    return returnType().isLifetimeConstant();
}

bool Expression::usesVar(const std::string& name) const
{
    prep();
    return _vars.find(name) != _vars.end();
}

bool Expression::usesFunc(const std::string& name) const
{
    prep();
    return _funcs.find(name) != _funcs.end();
}

void Expression::parse() const
{
    if (_parseTree)
        return;
    std::lock_guard<std::mutex> guard(_parseMutex);
    if (_parseTree)
        return;
    int tempStartPos, tempEndPos;
    ExprNode* parseTree_ = nullptr;
    bool OK = ExprParseAction(parseTree_, _parseError, tempStartPos, tempEndPos, _comments, this, _expression.c_str(),
                              _wantVec);
    if (!OK || !parseTree_) {
        addError(_parseError, tempStartPos, tempEndPos);
        delete parseTree_;
    } else {
        // TODO: need promote
        _parseTree = parseTree_;
        _returnType = parseTree_->type();
    }
}

void Expression::prep() const
{
    if (_evaluator)
        return;
    std::lock_guard<std::mutex> guard(_prepMutex);
    if (_evaluator)
        return;
#ifdef SEEXPR_PERFORMANCE
    PrintTiming timer("[ PREP     ] v2 prep time: ");
#endif
    parse();

    bool error = false;
    Evaluator* evaluator = nullptr;
    ExprVarEnvBuilder envBuilder;

    if (!_parseTree) {
        // parse error
        error = true;
    } else if (!parseTree()->prep(_desiredReturnType.isFP(1), envBuilder).isValid()) {
        // prep error
        error = true;
    } else if (!ExprType::valuesCompatible(parseTree()->type(), _desiredReturnType)) {
        // incompatible type error
        error = true;
        parseTree()->addError("Expression generated type " + parseTree()->type().toString() +
                              " incompatible with desired type " + _desiredReturnType.toString());
    } else {
        _returnType = parseTree()->type();
        // optimize for constant values - if we have a module of just one constant float, avoid using LLVM.
        //   Querying typing information during prep()-time is a bit difficult. For the most part, our type
        //   information is uninitialized until after prep()-time. The only exception is for constant,
        //   single floating point values.
        //
        // TODO: separate Object Representation (ExprNode) from ParseTree (which should just be cheap tokens)
        bool isConstant_ = _returnType.isLifetimeConstant() || (_parseTree && parseTree()->numChildren() == 1 &&
                                                                parseTree()->child(0)->type().isLifetimeConstant());
        EvaluationStrategy strategy = isConstant_ ? EvaluationStrategy::UseInterpreter : _evaluationStrategyHint;

        evaluator = (strategy == UseInterpreter) ? (Evaluator*)new Interpreter() : (Evaluator*)new LLVMEvaluator();
        evaluator->setDebugging(debugging);
        error = !evaluator->prep(parseTree(), _desiredReturnType);
    }

    if (error) {
        if (evaluator)
            delete evaluator;
        evaluator = nullptr;
        _returnType = ExprType().Error();

        // build line lookup table
        std::vector<int> lines;
        const char* start = _expression.c_str();
        const char* p = _expression.c_str();
        while (*p != 0) {
            if (*p == '\n')
                lines.push_back(static_cast<int>(p - start));
            p++;
        }
        lines.push_back(static_cast<int>(p - start));

        std::stringstream sstream;
        for (unsigned int i = 0; i < _errors.size(); i++) {
            auto lower = std::lower_bound(lines.begin(), lines.end(), _errors[i].startPos);
            int line = static_cast<int>(lower - lines.begin());
            int lineStart = line ? lines[line - 1] : 0;
            int col = _errors[i].startPos - lineStart;
            sstream << (line + 1) << ":" << col << ": error: " << _errors[i].error << std::endl;
        }
        _parseError = std::string(sstream.str());
    }

    if (debugging) {
        std::cerr << "parse error \n" << parseError() << std::endl;
    }

    if (!evaluator)
        evaluator = new NullEvaluator();
    _evaluator = evaluator;
    assert(_evaluator);
}

bool Expression::isVec() const
{
    return syntaxOK() ? parseTree()->isVec() : _wantVec;
}
const ExprType& Expression::returnType() const
{
    prep();
    return _returnType;
}

const double* Expression::evalFP(VarBlock* varBlock) const {
    prepIfNeeded();
    if (_isValid) {
        if (_evaluationStrategy == UseInterpreter) {
            _interpreter->eval(varBlock);
            return (varBlock && varBlock->threadSafe) ? &(varBlock->d[_returnSlot]) : &_interpreter->d[_returnSlot];
        } else {  // useLLVM
            return _llvmEvaluator->evalFP(varBlock);
        }
    }
    static double noCrash[16] = {};
    return noCrash;
}

void Expression::evalMultiple(VarBlock* varBlock, int outputVarBlockOffset, size_t rangeStart, size_t rangeEnd) const
{
    assert(varBlock);
    double* outputPtr = const_cast<double*&>(varBlock->Pointer(outputVarBlockOffset));
    evaluator()->evalMultiple(varBlock, outputPtr, rangeStart, rangeEnd);
}

const char* Expression::evalStr(VarBlock* varBlock) const {
    prepIfNeeded();
    if (_isValid) {
        if (_evaluationStrategy == UseInterpreter) {
            _interpreter->eval(varBlock);
            return (varBlock && varBlock->threadSafe) ? varBlock->s[_returnSlot] : _interpreter->s[_returnSlot];
        } else {  // useLLVM
            return _llvmEvaluator->evalStr(varBlock);
        }
    }
    return 0;
}
}  // end namespace SeExpr2/
