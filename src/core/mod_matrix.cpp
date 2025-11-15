#include "core/mod_matrix.h"

#include "core/mod_matrix_parameters.h"

#include <algorithm>
#include <mutex>

namespace
{
std::mutex gModMatrixMutex;
std::vector<ModMatrixAssignment> gAssignments;
int gNextAssignmentId = 1;

void updateNextAssignmentIdLocked()
{
    int maxId = 0;
    for (const auto& assignment : gAssignments)
    {
        if (assignment.id > maxId)
            maxId = assignment.id;
    }
    gNextAssignmentId = maxId + 1;
    if (gNextAssignmentId <= 0)
        gNextAssignmentId = 1;
}

} // namespace

std::vector<ModMatrixAssignment> modMatrixGetAssignments()
{
    std::scoped_lock lock(gModMatrixMutex);
    return gAssignments;
}

ModMatrixAssignment modMatrixCreateAssignment()
{
    std::scoped_lock lock(gModMatrixMutex);
    ModMatrixAssignment assignment;
    assignment.id = gNextAssignmentId++;
    if (gNextAssignmentId <= 0)
        gNextAssignmentId = assignment.id + 1;
    gAssignments.push_back(assignment);
    return assignment;
}

bool modMatrixUpdateAssignment(const ModMatrixAssignment& assignment)
{
    bool updated = false;
    {
        std::scoped_lock lock(gModMatrixMutex);
        auto it = std::find_if(gAssignments.begin(), gAssignments.end(), [&](const ModMatrixAssignment& value) {
            return value.id == assignment.id;
        });
        if (it == gAssignments.end())
            return false;

        *it = assignment;
        if (assignment.id >= gNextAssignmentId)
            gNextAssignmentId = assignment.id + 1;
        if (gNextAssignmentId <= 0)
            gNextAssignmentId = 1;
        updated = true;
    }

    if (updated)
        modMatrixApplyAssignment(assignment);
    return true;
}

bool modMatrixRemoveAssignment(int assignmentId)
{
    std::scoped_lock lock(gModMatrixMutex);
    auto it = std::remove_if(gAssignments.begin(), gAssignments.end(), [assignmentId](const ModMatrixAssignment& value) {
        return value.id == assignmentId;
    });
    if (it == gAssignments.end())
        return false;

    gAssignments.erase(it, gAssignments.end());
    updateNextAssignmentIdLocked();
    return true;
}

std::optional<ModMatrixAssignment> modMatrixGetAssignment(int assignmentId)
{
    std::scoped_lock lock(gModMatrixMutex);
    auto it = std::find_if(gAssignments.begin(), gAssignments.end(), [assignmentId](const ModMatrixAssignment& value) {
        return value.id == assignmentId;
    });
    if (it == gAssignments.end())
        return std::nullopt;
    return *it;
}

void modMatrixSetAssignments(const std::vector<ModMatrixAssignment>& assignments)
{
    {
        std::scoped_lock lock(gModMatrixMutex);
        gAssignments = assignments;
        updateNextAssignmentIdLocked();
    }
    modMatrixApplyAllAssignments();
}

void modMatrixClearAssignments()
{
    std::scoped_lock lock(gModMatrixMutex);
    gAssignments.clear();
    gNextAssignmentId = 1;
}

void modMatrixApplyAssignment(const ModMatrixAssignment& assignment)
{
    if (assignment.trackId <= 0)
        return;

    const ModParameterInfo* info = modMatrixGetParameterInfo(assignment.parameterIndex);
    if (!info || !info->setter)
        return;

    float value = modMatrixNormalizedToValue(assignment.normalizedAmount, *info);
    info->setter(assignment.trackId, value);
}

void modMatrixApplyAssignmentsForTrack(int trackId)
{
    if (trackId <= 0)
        return;

    auto assignments = modMatrixGetAssignments();
    for (const auto& assignment : assignments)
    {
        if (assignment.trackId == trackId)
            modMatrixApplyAssignment(assignment);
    }
}

void modMatrixApplyAllAssignments()
{
    auto assignments = modMatrixGetAssignments();
    for (const auto& assignment : assignments)
        modMatrixApplyAssignment(assignment);
}
