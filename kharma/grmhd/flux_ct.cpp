/* 
 *  File: flux_ct.cpp
 *  
 *  BSD 3-Clause License
 *  
 *  Copyright (c) 2020, AFD Group at UIUC
 *  All rights reserved.
 *  
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *  
 *  1. Redistributions of source code must retain the above copyright notice, this
 *     list of conditions and the following disclaimer.
 *  
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  
 *  3. Neither the name of the copyright holder nor the names of its
 *     contributors may be used to endorse or promote products derived from
 *     this software without specific prior written permission.
 *  
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 *  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 *  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "decs.hpp"

#include "parthenon/parthenon.hpp"

using namespace parthenon;

namespace GRMHD {

/**
 * Constrained transport.  Modify B-field fluxes to preserve divB==0 condition to machine precision per-step
 */
parthenon::TaskStatus FluxCT(std::shared_ptr<Container<Real>>& rc)
{
    FLAG("Flux CT");
    auto pmb = rc->GetBlockPointer();
    GridVars F1 = rc->Get("c.c.bulk.cons").flux[X1DIR];
    GridVars F2 = rc->Get("c.c.bulk.cons").flux[X2DIR];
    GridVars F3 = rc->Get("c.c.bulk.cons").flux[X3DIR];

    int n1 = pmb->cellbounds.ncellsi(IndexDomain::entire);
    int n2 = pmb->cellbounds.ncellsj(IndexDomain::entire);
    int n3 = pmb->cellbounds.ncellsk(IndexDomain::entire);
    GridScalar emf1("emf1", n3, n2, n1);
    GridScalar emf2("emf2", n3, n2, n1);
    GridScalar emf3("emf3", n3, n2, n1);

    IndexDomain domain = IndexDomain::entire;
    int is = pmb->cellbounds.is(domain), ie = pmb->cellbounds.ie(domain);
    int js = pmb->cellbounds.js(domain), je = pmb->cellbounds.je(domain);
    int ks = pmb->cellbounds.ks(domain), ke = pmb->cellbounds.ke(domain);
    pmb->par_for("flux_ct_emf", ks+1, ke, js+1, je, is+1, ie,
        KOKKOS_LAMBDA_3D {
            emf3(k, j, i) =  0.25 * (F1(prims::B2, k, j, i) + F1(prims::B2, k, j-1, i) - F2(prims::B1, k, j, i) - F2(prims::B1, k, j, i-1));
            emf2(k, j, i) = -0.25 * (F1(prims::B3, k, j, i) + F1(prims::B3, k-1, j, i) - F3(prims::B1, k, j, i) - F3(prims::B1, k, j, i-1));
            emf1(k, j, i) =  0.25 * (F2(prims::B3, k, j, i) + F2(prims::B3, k-1, j, i) - F3(prims::B2, k, j, i) - F3(prims::B2, k, j-1, i));
        }
    );

    // Rewrite EMFs as fluxes, after Toth
    // TODO split to cover more ground?
    pmb->par_for("flux_ct", ks, ke-1, js, je-1, is, ie-1,
        KOKKOS_LAMBDA_3D {
            F1(prims::B1, k, j, i) =  0.0;
            F1(prims::B2, k, j, i) =  0.5 * (emf3(k, j, i) + emf3(k, j+1, i));
            F1(prims::B3, k, j, i) = -0.5 * (emf2(k, j, i) + emf2(k+1, j, i));

            F2(prims::B1, k, j, i) = -0.5 * (emf3(k, j, i) + emf3(k, j, i+1));
            F2(prims::B2, k, j, i) =  0.0;
            F2(prims::B3, k, j, i) =  0.5 * (emf1(k, j, i) + emf1(k+1, j, i));

            F3(prims::B1, k, j, i) =  0.5 * (emf2(k, j, i) + emf2(k, j, i+1));
            F3(prims::B2, k, j, i) = -0.5 * (emf1(k, j, i) + emf1(k, j+1, i));
            F3(prims::B3, k, j, i) =  0.0;
        }
    );
    FLAG("CT Finished");

    return TaskStatus::complete;
}

}