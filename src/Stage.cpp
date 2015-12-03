/******************************************************************************
* Copyright (c) 2011, Michael P. Gerlek (mpg@flaxen.com)
*
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following
* conditions are met:
*
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in
*       the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of Hobu, Inc. or Flaxen Geo Consulting nor the
*       names of its contributors may be used to endorse or promote
*       products derived from this software without specific prior
*       written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
* OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
* AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
* OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
* OF SUCH DAMAGE.
****************************************************************************/

#include <pdal/GlobalEnvironment.hpp>
#include <pdal/Stage.hpp>
#include <pdal/SpatialReference.hpp>
#include <pdal/UserCallback.hpp>

#include "StageRunner.hpp"

#include <memory>

namespace pdal
{


Stage::Stage()
  : m_callback(new UserCallback), m_progressFd(-1)
{
    Construct();
}


/// Only add options if an option with the same name doesn't already exist.
///
/// \param[in] ops  Options to add.
///
void Stage::addConditionalOptions(const Options& opts)
{
    for (const auto& o : opts.getOptions())
        if (!m_options.hasOption(o.getName()))
            m_options.add(o);
}


void Stage::Construct()
{
    m_debug = false;
    m_verbose = 0;
}


void Stage::prepare(PointTableRef table)
{
    for (size_t i = 0; i < m_inputs.size(); ++i)
    {
        Stage *prev = m_inputs[i];
        prev->prepare(table);
    }
    l_processOptions(m_options);
    processOptions(m_options);
    l_initialize(table);
    initialize(table);
    addDimensions(table.layout());
    prepared(table);
}


PointViewSet Stage::execute(PointTableRef table)
{
    table.finalize();

    PointViewSet views;

    // If the inputs are empty, we're a reader.
    if (m_inputs.empty())
    {
        views.insert(PointViewPtr(new PointView(table)));
    }
    else
    {
        for (size_t i = 0; i < m_inputs.size(); ++i)
        {
            Stage *prev = m_inputs[i];
            PointViewSet temp = prev->execute(table);
            views.insert(temp.begin(), temp.end());
        }
    }

    PointViewSet outViews;
    std::vector<StageRunnerPtr> runners;

    // Put the spatial references from the views onto the table.
    // The table's spatial references are only valid as long as the stage
    // is running.
    // ABELL - Should we clear the references once the stage run has
    //   completed?  Wondering if that would break something where a
    //   writer wants to check a table's SRS.
    SpatialReference srs;
    table.clearSpatialReferences();
    for (auto const& it : views)
        table.addSpatialReference(it->spatialReference());

    // Do the ready operation and then start running all the views
    // through the stage.
    ready(table);
    for (auto const& it : views)
    {
        StageRunnerPtr runner(new StageRunner(this, it));
        runners.push_back(runner);
        runner->run();
    }

    // As the stages complete (synchronously at this time), propagate the
    // spatial reference and merge the output views.
    srs = getSpatialReference();
    for (auto const& it : runners)
    {
        StageRunnerPtr runner(it);
        PointViewSet temp = runner->wait();
        
        // If our stage has a spatial reference, the view takes it on once
        // the stage has been run.
        if (!srs.empty())
            for (PointViewPtr v : temp)
                v->setSpatialReference(srs);
        outViews.insert(temp.begin(), temp.end());
    }
    done(table);
    return outViews;
}


// Streamed execution.
void Stage::execute(StreamPointTable& table)
{
    table.finalize();

    std::list<Stage *> stages;
    std::list<Stage *> filters;
    std::vector<bool> skips(table.capacity());
    SpatialReference srs;

    // Build a list of the stages.
    Stage *s = this;
    while (true)
    {
        if (s->m_inputs.size() > 1)
            throw pdal_error("Can't execute streaming pipeline stages "
                "containing multiple inputs.");
        stages.push_front(s);
        if (s->m_inputs.empty())
            break;
        s = s->m_inputs[0];
    }

    // Separate out the first stage.
    Stage *reader = stages.front();

    // Build a list of all stages except the first.  We may have a writer in
    // this list in addition to filters, but we treat them in the same way.
    auto begin = stages.begin();
    begin++;
    std::copy(begin, stages.end(), std::back_inserter(filters));

    for (Stage *s : stages)
    {
        s->ready(table);
        srs = s->getSpatialReference();
        if (!srs.empty())
            table.setSpatialReference(srs);
    }

    // Loop until we're finished.  We handle the number of points up to
    // the capacity of the StreamPointTable that we've been provided.

    bool finished = false;
    while (!finished)
    {
        // Clear the spatial
        table.clearSpatialReferences();
        PointId idx = 0;
        PointRef point(&table, idx);
        point_count_t pointLimit = table.capacity();

        // When we get false back from a reader, we're done, so set
        // the point limit to the number of points processed in this loop
        // of the table.
        for (PointId idx = 0; idx < pointLimit; idx++)
        {
            point.setPointId(idx);
            finished = !reader->processOne(point);
            if (finished)
                pointLimit = idx;
        }
        srs = reader->getSpatialReference();
        if (!srs.empty())
            table.setSpatialReference(srs);

        // When we get a false back from a filter, we're filtering out a
        // point, so add it to the list of skips so that it doesn't get
        // processed by subsequent filters.
        for (Stage *s : filters)
        {
            for (PointId idx = 0; idx < pointLimit; idx++)
            {
                if (skips[idx])
                    continue;
                point.setPointId(idx);
                if (!s->processOne(point))
                    skips[idx] = true;
            }
            srs = s->getSpatialReference();
            if (!srs.empty())
                table.setSpatialReference(srs);
        }

        // Yes, vector<bool> is terrible.  Can do something better later.
        for (size_t i = 0; i < skips.size(); ++i)
            skips[i] = false;
        table.reset();
    }

    for (Stage *s : stages)
        s->done(table);
}


void Stage::l_initialize(PointTableRef table)
{
    m_metadata = table.metadata().add(getName());
}


void Stage::l_processOptions(const Options& options)
{
    m_debug = options.getValueOrDefault<bool>("debug", false);
    m_verbose = options.getValueOrDefault<uint32_t>("verbose", 0);
    if (m_debug && !m_verbose)
        m_verbose = 1;

    if (m_inputs.empty())
    {
        std::string logname =
            options.getValueOrDefault<std::string>("log", "stdlog");
        m_log = std::shared_ptr<pdal::Log>(new Log(getName(), logname));
    }
    else
    {
        if (options.hasOption("log"))
        {
            std::string logname = options.getValueOrThrow<std::string>("log");
            m_log.reset(new Log(getName(), logname));
        }
        else
        {
            // We know we're not empty at this point
            std::ostream* v = m_inputs[0]->log()->getLogStream();
            m_log.reset(new Log(getName(), v));
        }
    }
    m_log->setLevel((LogLevel::Enum)m_verbose);

    // If the user gave us an SRS via options, take that.
    try
    {
        m_spatialReference = options.
            getValueOrThrow<pdal::SpatialReference>("spatialreference");
    }
    catch (pdal_error const&)
    {
        // If one wasn't set on the options, we'll ignore at this
        // point.  Maybe another stage might forward/set it later.
    }

    // Process reader-specific options.
    readerProcessOptions(options);
    // Process writer-specific options.
    writerProcessOptions(options);
}


const SpatialReference& Stage::getSpatialReference() const
{
    return m_spatialReference;
}


void Stage::setSpatialReference(const SpatialReference& spatialRef)
{
    setSpatialReference(m_metadata, spatialRef);
}


void Stage::setSpatialReference(MetadataNode& m,
    const SpatialReference& spatialRef)
{
    m_spatialReference = spatialRef;

    auto pred = [](MetadataNode m){ return m.name() == "spatialreference"; };

    MetadataNode spatialNode = m.findChild(pred);
    if (spatialNode.empty())
    {
        m.add("spatialreference",
           spatialRef.getWKT(SpatialReference::eHorizontalOnly, false),
           "SRS of this stage");
        m.add("comp_spatialreference",
            spatialRef.getWKT(SpatialReference::eCompoundOK, false),
            "SRS of this stage");
    }
}

std::vector<Stage *> Stage::findStage(std::string name)
{
    std::vector<Stage *> output;

    if (boost::iequals(getName(), name))
        output.push_back(this);

    for (auto const& stage : m_inputs)
    {
        if (boost::iequals(stage->getName(), name))
            output.push_back(stage);
        if (stage->getInputs().size())
        {
            auto hits = stage->findStage(name);
            if (hits.size())
                output.insert(output.end(), hits.begin(), hits.end());
        }
    }

    return output;
}

std::ostream& operator<<(std::ostream& ostr, const Stage& stage)
{
    ostr << "  Name: " << stage.getName() << std::endl;
    ostr << "  Spatial Reference:" << std::endl;
    ostr << "    WKT: " << stage.getSpatialReference().getWKT() << std::endl;

    return ostr;
}

} // namespace pdal
