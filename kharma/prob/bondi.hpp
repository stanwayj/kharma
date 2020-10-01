/*
 * Bondi flow functions
 */

#include "decs.hpp"

#include "gr_coordinates.hpp"
#include "eos.hpp"

#include "interface/container.hpp"

/**
 * Initialization of a Bondi problem with specified sonic point, BH mdot, and horizon radius
 * TODO this can/should be just mdot (and the grid ofc), if this problem is to be used as anything more than a test
 */
void InitializeBondi(std::shared_ptr<MeshBlock> pmb, const GRCoordinates& G, GridVars P,
                     const EOS* eos, const Real mdot, const Real rs);

/**
 * Apply the Bondi flow condition on right X1 boundary
 */
void ApplyBondiBoundary(std::shared_ptr<Container<Real>>& rc);
