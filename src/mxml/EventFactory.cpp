//  Created by Alejandro Isaza on 2014-05-15.
//  Copyright (c) 2014 Venture Media Labs. All rights reserved.

#include "EventFactory.h"

#include <mxml/dom/Backup.h>
#include <mxml/dom/Barline.h>
#include <mxml/dom/Chord.h>
#include <mxml/dom/Direction.h>
#include <mxml/dom/Forward.h>
#include <mxml/dom/Part.h>
#include <mxml/dom/TimedNode.h>
#include <mxml/dom/Types.h>

namespace mxml {

using namespace dom;

EventFactory::EventFactory(const dom::Score& score)
: _score(score), _measureIndex(0), _time(0), _loopBegin(0),  _endingBegin(0), _eventSequence()
{}

const EventSequence& EventFactory::build() {
    _eventSequence.clear();
    _firstPass = true;

    for (auto& part : _score.parts()) {
        _part = part.get();
        _measureStartTime = 0;
        _time = 0;
        for (_measureIndex = 0; _measureIndex < part->measures().size(); _measureIndex += 1) {
            const Measure& measure = *part->measures().at(_measureIndex);
            processMeasure(measure);
        }
        _firstPass = false;
    }

    // Fill wall times
    double tempo = 60.0;
    int divisions = 1;

    dom::time_t time = 0;
    double wallTime = 0.0;

    auto attributesIter = _eventSequence.attributes().begin();
    auto attributesEndIter = _eventSequence.attributes().end();
    auto attributesNextIter = attributesIter;
    if (attributesIter != attributesEndIter) {
        if (attributesIter->begin <= time)
            divisions = attributesIter->attributes->divisions();
        ++attributesNextIter;
    }

    auto temposIter = _eventSequence.tempos().begin();
    auto temposEndIter = _eventSequence.tempos().end();
    auto temposNextIter = temposIter;
    if (temposIter != temposEndIter) {
        if (temposIter->begin <= time)
            tempo = temposIter->value;
        ++temposNextIter;
    }

    for (Event& event : _eventSequence.events()) {
        if (attributesNextIter != attributesEndIter && attributesNextIter->begin <= time) {
            attributesIter = attributesNextIter;
            divisions = attributesIter->attributes->divisions();
            ++attributesNextIter;
        }

        if (temposNextIter != temposEndIter && temposNextIter->begin <= time) {
            temposIter = temposNextIter;
            ++temposNextIter;
            tempo = temposIter->value;
        }

        double divisionDuration = 60.0 / (divisions * tempo);
        wallTime += divisionDuration * static_cast<double>(event.time() - time);
        time = event.time();

        event.setWallTime(wallTime);
        event.setWallTimeDuration(event.maxDuration() * divisionDuration);
    }

    return _eventSequence;
}

int EventFactory::currentDivisions() const {
    auto attributes = _eventSequence.attributes(_measureStartTime);
    if (attributes)
        return attributes->divisions();
    return 1;
}

dom::Time EventFactory::currentTime() const {
    auto attributes = _eventSequence.attributes(_measureStartTime);
    if (attributes)
        return attributes->time();
    return dom::Time{};
}

void EventFactory::processMeasure(const dom::Measure& measure) {
    for (auto& node : measure.nodes()) {
        if (node == measure.nodes().back() && !dynamic_cast<const TimedNode*>(node.get()) && _eventSequence.attributes(_time))
            _time = _measureStartTime + currentDivisions() * currentTime().beats();;

        if (const Barline* barline = dynamic_cast<const Barline*>(node.get())) {
            if (_firstPass)
                processBarline(*barline);
        } else if (const Attributes* attributes = dynamic_cast<const Attributes*>(node.get())) {
            processAttributes(*attributes);
        } else if (const Direction* direction = dynamic_cast<const Direction*>(node.get())) {
            processDirection(*direction);
        } else if (const TimedNode* timedNode = dynamic_cast<const TimedNode*>(node.get())) {
            processTimedNode(*timedNode);
        }
    }

    _measureStartTime += Attributes::divisionsPerMeasure(currentDivisions(), currentTime());
    _time = _measureStartTime;
}

void EventFactory::processBarline(const dom::Barline& barline) {
    if (barline.repeat().isPresent()) {
        const Repeat& repeat = barline.repeat();
        if (repeat.direction() == Repeat::DIRECTION_FORWARD) {
            _loopBegin = _time;
        } else {
            EventSequence::Loop loop;
            loop.begin = _loopBegin;
            loop.end = _time;
            loop.count = 1;
            _eventSequence.addLoop(loop);
            _loopBegin = _time;
        }
    }
    
    if (barline.ending().isPresent()) {
        const Ending& ending = barline.ending();
        if (ending.type() == Ending::START) {
            _endingBegin = _time;
        } else {
            EventSequence::Ending ee;
            ee.begin = _endingBegin;
            ee.end = _time;
            ee.numbers = ending.numbers();
            _eventSequence.addEnding(ee);
        }
        
        if (ending.type() == Ending::DISCONTINUE && !_eventSequence.loops().empty())
            _eventSequence.loops().back().count = *ending.numbers().rbegin() - 1;
    }
}

void EventFactory::processAttributes(const dom::Attributes& attributes) {
    EventSequence::Attributes attr;
    attr.begin = _time;
    attr.part = _part;
    attr.attributes = &attributes;
    _eventSequence.addAttributes(attr);
}

void EventFactory::processDirection(const dom::Direction& direction) {
    if (!direction.sound().isPresent())
        return;
    const Sound& sound = direction.sound();
    
    if (sound.tempo().isPresent()) {
        EventSequence::Value value;
        value.begin = _time;
        value.value = sound.tempo().value();
        _eventSequence.addTempo(value);
    }
    
    if (sound.dynamics().isPresent()) {
        EventSequence::Value value;
        value.begin = _time;
        value.part = _part;
        value.value = sound.dynamics().value();
        _eventSequence.addDynamics(value);
    }
}

void EventFactory::processTimedNode(const TimedNode& node) {
    if (auto chord = dynamic_cast<const Chord*>(&node)) {
        processChord(*chord);
        _time += node.duration();
    } else if (auto note = dynamic_cast<const Note*>(&node)) {
        addNote(*note);
        _time += node.duration();
    } else if (dynamic_cast<const Forward*>(&node)) {
        _time += node.duration();
    } else if (dynamic_cast<const Backup*>(&node)) {
        _time -= node.duration();
    }
}

void EventFactory::processChord(const Chord& chord) {
    for (auto& note : chord.notes())
        addNote(*note);
}

void EventFactory::addNote(const Note& note) {
    Event* onEvent = _eventSequence.event(_time);
    if (!onEvent)
        onEvent = &_eventSequence.addEvent(Event(_score, _measureIndex, _time));
    onEvent->setMeasureIndex(_measureIndex);
    onEvent->addOnNote(note);

    Event* offEvent = _eventSequence.event(_time + note.duration());
    if (!offEvent)
        offEvent = &_eventSequence.addEvent(Event(_score, _measureIndex, _time + note.duration()));
    offEvent->addOffNote(note);
}

} // namespace mxml
