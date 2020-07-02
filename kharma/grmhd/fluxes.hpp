// Support functions for calculating & correcting fluxes
#pragma once

#include "mesh/mesh.hpp"
#include "coordinates/coordinates.hpp"

#include "decs.hpp"

#include "reconstruction.hpp"
#include "phys.hpp"

using namespace parthenon;

/**
 * Take reconstructed primitives at either side of a face and construct the local Lax-Friedrichs flux
 *
 * Also fills the "ctop" vector with the highest magnetosonic speed mhd_vchar -- used to estimate timestep later.
 *
 * Note that since this L and R are defined with respect to the *face*, they are actually the
 * opposite of the "r" and "l" in the caller, CalculateFluxes!
 */
void LRToFlux(std::shared_ptr<Container<Real>>& rc, GridVars pl, GridVars pr, const int dir, GridVars flux)
{
    FLAG("LR to flux");
    MeshBlock *pmb = rc->pmy_block;
    IndexDomain domain = IndexDomain::interior;
    int is = pmb->cellbounds.is(domain), ie = pmb->cellbounds.ie(domain);
    int js = pmb->cellbounds.js(domain), je = pmb->cellbounds.je(domain);
    int ks = pmb->cellbounds.ks(domain), ke = pmb->cellbounds.ke(domain);
    int n1 = pmb->cellbounds.ncellsi(IndexDomain::entire);
    int n2 = pmb->cellbounds.ncellsj(IndexDomain::entire);
    int n3 = pmb->cellbounds.ncellsk(IndexDomain::entire);

    GRCoordinates G = pmb->coords;

    //GridVars pll("pll", NPRIM, n3, n2, n1);

    auto& ctop = rc->GetFace("f.f.bulk.ctop").data;

    // So far we don't need fluxes that don't match faces
    Loci loc;

    // TODO don't construct EOSes.  Somehow.
    Real gamma = pmb->packages["GRMHD"]->Param<Real>("gamma");
    EOS* eos = CreateEOS(gamma);

    // Offset "left" variables by one zone to line up L- and R-fluxes at *faces*
    switch (dir) {
    case 1:
        // pmb->par_for("offset_left_1", 0, NPRIM-1, ks-1, ke+1, js-1, je+1, is-1, ie+1,
        //     KOKKOS_LAMBDA_VARS {
        //         pll(p, k, j, i) = pl(p, k, j, i-1);
        //     }
        // );
        loc = Loci::face1;
        break;
    case 2:
        // pmb->par_for("offset_left_2", 0, NPRIM-1, ks-1, ke+1, js-1, je+1, is-1, ie+1,
        //     KOKKOS_LAMBDA_VARS {
        //         pll(p, k, j, i) = pl(p, k, j-1, i);
        //     }
        // );
        loc = Loci::face2;
        break;
    case 3:
        // pmb->par_for("offset_left_3", 0, NPRIM-1, ks-1, ke+1, js-1, je+1, is-1, ie+1,
        //     KOKKOS_LAMBDA_VARS {
        //         pll(p, k, j, i) = pl(p, k-1, j, i);
        //     }
        // );
        loc = Loci::face3;
        break;
#if DEBUG
    // TODO Throw an error
#endif
    }
    //FLAG("Left offset");

    //  LOOP FUSION BABY
    pmb->par_for("uber_flux", ks-1, ke+1, js-1, je+1, is-1, ie+1,
            KOKKOS_LAMBDA_3D {
                FourVectors Dtmp;
                Real cmaxL, cmaxR, cminL, cminR;
                Real cmin, cmax, ctop_loc;

                Real fluxL[8], fluxR[8];
                Real Ul[8], Ur[8];

                // All the following calls write to *local* temporaries.
                // That means we don't need an offset-left array to make indices line up
                // We can just *read* from a different spot
                int kl, jl, il;
                switch (dir) {
                case 1:
                    kl = k; jl = j; il = i - 1;
                    break;
                case 2:
                    kl = k; jl = j - 1; il = i;
                    break;
                case 3:
                    kl = k - 1; jl = j; il = i;
                    break;
                }

                // Left
                get_state(G, pl, kl, jl, il, loc, Dtmp);
                prim_to_flux(G, pl, Dtmp, eos, kl, jl, il, loc, 0, Ul); // dir==0 -> U instead of F in direction
                prim_to_flux(G, pl, Dtmp, eos, kl, jl, il, loc, dir, fluxL);
                mhd_vchar(G, pl, Dtmp, eos, kl, jl, il, loc, dir, cmaxL, cminL);

                // Right
                get_state(G, pr, k, j, i, loc, Dtmp);
                // Note: these three can be done simultaneously if we want to get real fancy
                prim_to_flux(G, pr, Dtmp, eos, k, j, i, loc, 0, Ur);
                prim_to_flux(G, pr, Dtmp, eos, k, j, i, loc, dir, fluxR);
                mhd_vchar(G, pr, Dtmp, eos, k, j, i, loc, dir, cmaxR, cminR);

                cmax = fabs(max(max(0.,  cmaxL),  cmaxR));
                cmin = fabs(max(max(0., -cminL), -cminR));
                ctop_loc = max(cmax, cmin);

                ctop(dir, k, j, i) = ctop_loc;
                PLOOP flux(p, k, j, i) = 0.5 * (fluxL[p] + fluxR[p] - ctop_loc * (Ur[p] - Ul[p]));
            }
    );
    
    DelEOS(eos);

    FLAG("Uber fluxcalc");
}
