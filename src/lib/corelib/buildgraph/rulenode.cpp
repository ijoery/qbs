/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qbs.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "rulenode.h"

#include "buildgraph.h"
#include "buildgraphvisitor.h"
#include "productbuilddata.h"
#include "projectbuilddata.h"
#include "rulesapplicator.h"
#include "transformer.h"
#include "transformerchangetracking.h"

#include <language/language.h>
#include <logging/categories.h>
#include <logging/logger.h>
#include <tools/qbsassert.h>
#include <tools/qttools.h>

namespace qbs {
namespace Internal {

RuleNode::RuleNode()
{
}

RuleNode::~RuleNode()
{
}

void RuleNode::accept(BuildGraphVisitor *visitor)
{
    if (visitor->visit(this))
        acceptChildren(visitor);
    visitor->endVisit(this);
}

QString RuleNode::toString() const
{
    return QLatin1String("RULE ") + m_rule->toString() + QLatin1String(" [")
            + (!product.expired() ? product->name : QLatin1String("<null>")) + QLatin1Char(']')
            + QLatin1String(" located at ") + m_rule->prepareScript.location().toString();
}

void RuleNode::apply(const Logger &logger, const ArtifactSet &changedInputs,
                     const std::unordered_map<QString, const ResolvedProduct *> &productsByName,
                     const std::unordered_map<QString, const ResolvedProject *> &projectsByName,
                     ApplicationResult *result)
{
    ArtifactSet allCompatibleInputs = currentInputArtifacts();
    const ArtifactSet addedInputs = allCompatibleInputs - m_oldInputArtifacts;
    const ArtifactSet removedInputs = m_oldInputArtifacts - allCompatibleInputs;
    result->upToDate = changedInputs.empty() && addedInputs.empty() && removedInputs.empty();

    qCDebug(lcBuildGraph).noquote().nospace()
            << "consider " << (m_rule->isDynamic() ? "dynamic " : "")
            << (m_rule->multiplex ? "multiplex " : "")
            << "rule node " << m_rule->toString()
            << "\n\tchanged: " << changedInputs.toString()
            << "\n\tcompatible: " << allCompatibleInputs.toString()
            << "\n\tadded: " << addedInputs.toString()
            << "\n\tremoved: " << removedInputs.toString();

    ArtifactSet inputs = changedInputs;
    if (m_rule->multiplex)
        inputs = allCompatibleInputs;
    else
        inputs += addedInputs;

    for (Artifact * const input : allCompatibleInputs) {
        for (const Artifact * const output : input->parentArtifacts()) {
            if (output->transformer->rule != m_rule)
                continue;
            if (prepareScriptNeedsRerun(output->transformer.get(),
                                        output->transformer->product().get(),
                                        productsByName, projectsByName)) {
                result->upToDate = false;
                inputs += input;
            }
            break;
        }
        if (m_rule->multiplex)
            break;
    }

    // Handle rules without inputs: We want to run such a rule if and only if it has not run yet
    // or its transformer is not up to date regarding the prepare script.
    if (result->upToDate && (!m_rule->declaresInputs() || !m_rule->requiresInputs)
            && inputs.empty()) {
        bool hasOutputs = false;
        for (const Artifact * const output : filterByType<Artifact>(parents)) {
            if (output->transformer->rule != m_rule)
                continue;
            hasOutputs = true;
            if (prepareScriptNeedsRerun(output->transformer.get(),
                                        output->transformer->product().get(),
                                        productsByName, projectsByName)) {
                result->upToDate = false;
                break;
            }
            if (m_rule->multiplex)
                break;
        }
        if (!hasOutputs)
            result->upToDate = false;
    }

    if (result->upToDate)
        return;
    if (!removedInputs.empty()) {
        ArtifactSet outputArtifactsToRemove;
        for (Artifact * const artifact : removedInputs) {
            for (Artifact *parent : filterByType<Artifact>(artifact->parents)) {
                if (parent->transformer->rule != m_rule) {
                    // parent was not created by our rule.
                    continue;
                }

                // parent must always have a transformer, because it's generated.
                QBS_CHECK(parent->transformer);

                // artifact is a former input of m_rule and parent was created by m_rule
                // the inputs of the transformer must contain artifact
                QBS_CHECK(parent->transformer->inputs.contains(artifact));

                outputArtifactsToRemove += parent;
            }
        }
        RulesApplicator::handleRemovedRuleOutputs(inputs, outputArtifactsToRemove, logger);
    }
    if (!inputs.empty() || !m_rule->declaresInputs() || !m_rule->requiresInputs) {
        RulesApplicator applicator(product.lock(), productsByName, projectsByName, logger);
        applicator.applyRule(m_rule, inputs);
        result->createdNodes = applicator.createdArtifacts();
        result->invalidatedNodes = applicator.invalidatedArtifacts();
    }
    m_oldInputArtifacts = inputs;
    product->topLevelProject()->buildData->setDirty();
}

void RuleNode::load(PersistentPool &pool)
{
    BuildGraphNode::load(pool);
    serializationOp<PersistentPool::Load>(pool);
}

void RuleNode::store(PersistentPool &pool)
{
    BuildGraphNode::store(pool);
    serializationOp<PersistentPool::Store>(pool);
}

ArtifactSet RuleNode::currentInputArtifacts() const
{
    ArtifactSet s;
    for (const FileTag &t : qAsConst(m_rule->inputs)) {
        for (Artifact *artifact : product->lookupArtifactsByFileTag(t)) {
            if (artifact->isTargetOfModule())
                continue;
            if (artifact->transformer && artifact->transformer->rule == m_rule) {
                // Do not add compatible artifacts as inputs that were created by this rule.
                // This can e.g. happen for the ["cpp", "hpp"] -> ["hpp", "cpp", "unmocable"] rule.
                continue;
            }
            s += artifact;
        }
    }

    if (m_rule->inputsFromDependencies.empty())
        return s;
    for (const FileTag &t : qAsConst(m_rule->inputsFromDependencies)) {
        for (Artifact *artifact : product->lookupArtifactsByFileTag(t)) {
            if (!artifact->isTargetOfModule())
                continue;
            if (artifact->transformer && artifact->transformer->rule == m_rule)
                continue;
            if (artifact->fileTags().intersects(m_rule->excludedInputs))
                continue;
            s += artifact;
        }
    }

    for (const ResolvedProductConstPtr &dep : qAsConst(product->dependencies)) {
        if (!dep->buildData)
            continue;
        for (Artifact * const a : filterByType<Artifact>(dep->buildData->allNodes())) {
            if (a->fileTags().intersects(m_rule->inputsFromDependencies)
                    && !a->fileTags().intersects(m_rule->excludedInputs))
                s += a;
        }
    }

    return s;
}

} // namespace Internal
} // namespace qbs
