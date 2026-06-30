#pragma once

// Internal (USD-aware) header shared *within* the tracey_usd library only. Public
// consumers use the USD-free usd_loader.hpp / usd_exporter.hpp / stage_document.hpp.

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>

#include <memory>
#include <string>

namespace tracey
{
    class Scene;

    // Convert an already-open, composed USD stage into a tracey::Scene. This is the
    // shared body of UsdLoader::loadFromFile (which opens a path then calls this) and
    // StageDocument::toScene / toSceneAtTime (which pass its retained, edited stage).
    // `sourceLabel` is only used in the import log line. `time` selects the time code
    // at which transforms are evaluated (Default = static; a frame number = animated).
    std::unique_ptr<Scene> convertStageToScene(const pxr::UsdStageRefPtr &stage,
                                               const std::string &sourceLabel,
                                               const pxr::UsdTimeCode &time = pxr::UsdTimeCode::Default());
} // namespace tracey
