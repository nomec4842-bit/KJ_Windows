#pragma once

#include <optional>
#include <vector>

struct ModMatrixAssignment
{
    int id = 0;
    int sourceIndex = 0;
    int trackId = 0;
    int parameterIndex = 0;
    float normalizedAmount = 0.0f;
};

std::vector<ModMatrixAssignment> modMatrixGetAssignments();
ModMatrixAssignment modMatrixCreateAssignment();
bool modMatrixUpdateAssignment(const ModMatrixAssignment& assignment);
bool modMatrixRemoveAssignment(int assignmentId);
std::optional<ModMatrixAssignment> modMatrixGetAssignment(int assignmentId);
void modMatrixSetAssignments(const std::vector<ModMatrixAssignment>& assignments);
void modMatrixClearAssignments();
void modMatrixApplyAssignment(const ModMatrixAssignment& assignment);
void modMatrixApplyAssignmentsForTrack(int trackId);
void modMatrixApplyAllAssignments();
