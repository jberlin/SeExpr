/*
* Copyright Disney Enterprises, Inc.  All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License
* and the following modification to it: Section 6 Trademarks.
* deleted and replaced with:
*
* 6. Trademarks. This License does not grant permission to use the
* trade names, trademarks, service marks, or product names of the
* Licensor and its affiliates, except as required for reproducing
* the content of the NOTICE file.
*
* You may obtain a copy of the License at
* http://www.apache.org/licenses/LICENSE-2.0
*
* @file ExprCompletionModel.h
* @brief This provides an expression editor for SeExpr syntax with auto ui features
* @author  aselle
*/
#include <QLineEdit>
#include <SeExpr2/Expression.h>
#include <SeExpr2/ExprFunc.h>
#include "ExprCompletionModel.h"

std::vector<QString> ExprCompletionModel::builtins;

ExprCompletionModel::ExprCompletionModel(QObject* parent) : QAbstractItemModel(parent)
{
    if (builtins.size() == 0) {
        std::vector<std::string> builtins_std;
        SeExpr2::ExprFunc::getFunctionNames(builtins_std);
        for (unsigned int i = 0; i < builtins_std.size(); i++)
            builtins.push_back(QString(builtins_std[i].c_str()));
    }
}

void ExprCompletionModel::clearVariables()
{
    variables.clear();
    variables_comment.clear();
}

void ExprCompletionModel::addVariable(const QString& str, const QString& comment)
{
    if (str.at(0) == '$') {
        variables.push_back(str);
        variables_comment.push_back(comment);
        QString noDollar(str);
        noDollar.remove(0, 1);
        variables.push_back(noDollar);
        variables_comment.push_back(comment);
    } else {
        QString dollar(str);
        dollar.insert(0, '$');
        variables.push_back(str);
        variables_comment.push_back(comment);
        variables.push_back(str);
        variables_comment.push_back(comment);
    }
}

void ExprCompletionModel::clearFunctions()
{
    functions.clear();
    functions_comment.clear();
    functionNameToFunction.clear();
}

void ExprCompletionModel::addFunction(const QString& str, const QString& comment)
{
    functionNameToFunction[str] = functions_comment.size();
    functions.push_back(str);
    functions_comment.push_back(comment);
}

void ExprCompletionModel::syncExtras(const ExprCompletionModel& otherModel)
{
    functionNameToFunction = otherModel.functionNameToFunction;
    functions = otherModel.functions;
    functions_comment = otherModel.functions_comment;
    variables = otherModel.variables;
    variables_comment = otherModel.variables_comment;
}

QVariant ExprCompletionModel::data(const QModelIndex& index, int role) const
{
    static QColor variableColor = QColor(100, 200, 250), functionColor = QColor(100, 250, 200),
                  backgroundColor(50, 50, 50);

    if (!index.isValid())
        return QVariant();
    int row = index.row(), column = index.column();

    int local_variables_offset = variables.size();
    int builtin_funcs_offset = local_variables_offset + local_variables.size();
    int local_funcs_offset = builtin_funcs_offset + builtins.size();

    if (role == Qt::BackgroundRole)
        return backgroundColor;

    if (role == Qt::FontRole && column == 0) {
        QFont font;
        font.setBold(true);
        return font;
    }

    if (row < local_variables_offset) {
        int index = row;
        if (role == Qt::DisplayRole || role == Qt::EditRole) {
            if (column == 0)
                return QVariant(variables[index]);
            else if (column == 1)
                return QVariant(variables_comment[index]);
        } else if (role == Qt::ForegroundRole) {
            return variableColor;
        } else if (role == Qt::UserRole) {
            return QVariant(false);
        }
    } else if (row < builtin_funcs_offset) {
        int index = row - local_variables_offset;
        if (role == Qt::DisplayRole || role == Qt::EditRole) {
            if (column == 0)
                return QVariant(local_variables[index]);
            else if (column == 1)
                return QVariant("Local");
        } else if (role == Qt::ForegroundRole) {
            return variableColor;
        } else if (role == Qt::UserRole) {
            return QVariant(false);
        }
    } else if (row < local_funcs_offset) {
        int index = row - builtin_funcs_offset;
        if (role == Qt::DisplayRole || role == Qt::EditRole) {
            if (column == 0)
                return QVariant(builtins[index]);
            else if (column == 1)
                return QVariant(getFirstLine(SeExpr2::ExprFunc::getDocString(builtins[index].toStdString().c_str())));
        } else if (role == Qt::ForegroundRole) {
            return functionColor;  // darkGreen;
        } else if (role == Qt::UserRole) {
            return QVariant(true);
        }
    } else if (row < local_funcs_offset + functions.size()) {
        int index = row - local_funcs_offset;
        if (role == Qt::DisplayRole || role == Qt::EditRole) {
            if (column == 0)
                return QVariant(functions[index]);
            else if (column == 1)
                return QVariant(getFirstLine(functions_comment[index].toStdString()));
        } else if (role == Qt::ForegroundRole) {
            return functionColor;  // darkGreen;
        } else if (role == Qt::UserRole) {
            return QVariant(true);
        }
    }
    return QVariant();
}

QString ExprCompletionModel::getDocString(const QString& s)
{
    std::map<QString, int>::iterator i = functionNameToFunction.find(s);
    if (i != functionNameToFunction.end()) {
        return functions_comment[i->second];
    } else
        return SeExpr2::ExprFunc::getDocString(s.toStdString().c_str()).c_str();
}
