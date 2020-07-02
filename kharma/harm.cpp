/*
 * HARM driver-specific things -- i.e. call the GRMHD physics module in
 * the correct RK2 LLF steps we know and love
 */

#include <iostream>

#include "parthenon_manager.hpp"
#include "bvals/boundary_conditions.hpp"
#include "bvals/bvals.hpp"
#include "driver/multistage.hpp"

#include "decs.hpp"

#include "bondi.hpp"
#include "boundaries.hpp"
#include "containers.hpp"
#include "fixup.hpp"
#include "grmhd.hpp"
#include "harm.hpp"

// Parthenon requires we override certain things
namespace parthenon {

    Packages_t ParthenonManager::ProcessPackages(std::unique_ptr<ParameterInput>& pin) {
        Packages_t packages;

        // Turn off GRMHD only if set to false in input file
        bool do_grmhd = pin->GetOrAddBoolean("Packages", "GRMHD", true);
        bool do_grhd = pin->GetOrAddBoolean("Packages", "GRHD", false);
        bool do_electrons = pin->GetOrAddBoolean("Packages", "howes_electrons", false);

        // enable other packages as needed
        bool do_scalars = pin->GetOrAddBoolean("Packages", "scalars", false);

        // Just one base package: integrated B-fields, or not.
        if (do_grmhd) {
            packages["GRMHD"] = GRMHD::Initialize(pin.get());
        } else if (do_grhd) {

        }

        // Scalars can be added 
        // if (do_scalars) {
        //     packages["scalars"] = BetterScalars::Initialize(pin.get());
        // }

        // TODO electrons, like scalars but w/heating step...

        return std::move(packages);
    }

} // namespace parthenon

/**
 * All the tasks which constitute advancing the fluid in a mesh block by a stage.
 * This includes calculation of necessary derived variables, reconstruction, calculation of fluxes,
 * Application of fluxes and a source term to update zones, and finally calculation of the next
 * timestep.
 * 
 * This section is heavily documented to avoid bugs.
 */
TaskList HARMDriver::MakeTaskList(MeshBlock *pmb, int stage)
{
    TaskList tl;

    TaskID none(0);
    // Parthenon separates out stages of higher-order integrators with "containers"
    // (a bundle of arrays capable of holding all Fields in the FluidState)
    // One container per stage, filled and used to update the base container over the course of the step
    // Additionally an accumulator dUdt is provided to temporarily store this stage's contribution to the RHS
    // TODO: I believe the base container is guaranteed to hold last step's product until the end of this step,
    // but need to check this.
    if (stage == 1) {
        auto& base = pmb->real_containers.Get();
        pmb->real_containers.Add("dUdt", base);
        for (int i=1; i<integrator->nstages; i++)
            pmb->real_containers.Add(stage_name[i], base);
    }

    // pull out the container we'll use to get fluxes and/or compute RHSs
    auto& sc0  = pmb->real_containers.Get(stage_name[stage-1]);
    // pull out a container we'll use to store dU/dt.
    auto& dudt = pmb->real_containers.Get("dUdt");
    // pull out the container that will hold the updated state
    auto& sc1  = pmb->real_containers.Get(stage_name[stage]);

    // TODO what does this do exactly?
    auto t_start_recv = tl.AddTask(Container<Real>::StartReceivingTask, none, sc1);

    // Calculate the LLF fluxes in each direction
    // This uses the primitives (P) to calculate fluxes to update the conserved variables (U)
    // Hence the two should reflect *exactly* the same fluid state, which I'll term "lockstep"
    auto t_calculate_flux1 = tl.AddTask(GRMHD::CalculateFlux1, t_start_recv, sc0);
    auto t_calculate_flux2 = tl.AddTask(GRMHD::CalculateFlux2, t_start_recv, sc0);
    auto t_calculate_flux3 = tl.AddTask(GRMHD::CalculateFlux3, t_start_recv, sc0);
    auto t_calculate_flux = t_calculate_flux1 | t_calculate_flux2 | t_calculate_flux3;
    
    auto t_flux_ct = tl.AddTask(GRMHD::FluxCT, t_calculate_flux, sc0);

    // Exchange flux corrections due to AMR and physical boundaries
    // Note this does NOT fix vector components since we bundle primitives
    // TODO skip these if not SMR/AMR i.e. refinement=none or something like that
    auto t_send_flux = tl.AddTask(Container<Real>::SendFluxCorrectionTask,
                                    t_flux_ct, sc0);
    auto t_recv_flux = tl.AddTask(Container<Real>::ReceiveFluxCorrectionTask,
                                    t_flux_ct, sc0);

    // TODO HARM's fix_flux for vector components

    // Apply fluxes to create a single update dU/dt
    auto t_flux_divergence = tl.AddTask(Update::FluxDivergence, t_recv_flux, sc0, dudt);
    auto t_source_term = tl.AddTask(GRMHD::SourceTerm, t_flux_divergence, sc0, dudt);
    // Apply dU/dt to the stage's initial state sc0 to obtain the stage final state sc1
    // Note this *only fills U* of sc1, so sc1 is out of lockstep
    auto t_update_container = tl.AddTask(UpdateContainer, t_source_term, pmb, stage, stage_name, integrator);

    // Update ghost cells.  Only performed on U of sc1
    auto t_send = tl.AddTask(Container<Real>::SendBoundaryBuffersTask,
                                t_update_container, sc1);
    auto t_recv = tl.AddTask(Container<Real>::ReceiveBoundaryBuffersTask,
                                t_update_container, sc1);
    auto t_fill_from_bufs = tl.AddTask(Container<Real>::SetBoundariesTask,
                                            t_recv, sc1);
    auto t_clear_comm_flags = tl.AddTask(Container<Real>::ClearBoundaryTask,
                                            t_fill_from_bufs, sc1);

    auto t_prolong_bound = tl.AddTask([](MeshBlock *pmb) {
        pmb->pbval->ProlongateBoundaries(0.0, 0.0);
        return TaskStatus::complete;
    }, t_fill_from_bufs, pmb);

    // Set physical boundaries
    // ApplyCustomBoundaries is only used for the Bondi test problem outer bound
    // Note custom boundaries must but need only update U.
    // TODO add physical inflow check to ApplyCustomBoundaries
    auto t_set_parthenon_bc = tl.AddTask(parthenon::ApplyBoundaryConditions,
                                            t_prolong_bound, sc1);
    auto t_set_custom_bc = tl.AddTask(ApplyCustomBoundaries, t_set_parthenon_bc, sc1);

    // Fill primitives, bringing U and P back into lockstep
    auto t_fill_derived = tl.AddTask(parthenon::FillDerivedVariables::FillDerived,
                                        t_set_custom_bc, sc1);

    // Apply floor values to sc1.  Note that this must take valid U and give valid U,P
    // TODO verify we don't need this, or that we don't need the counterpart in FillDerived
    //auto apply_floors = AddContainerTask(tl, ApplyFloors, fill_derived, sc1);

    // estimate next time step
    if (stage == integrator->nstages) {
        auto new_dt = tl.AddTask(
            [](std::shared_ptr<Container<Real>> &rc) {
                MeshBlock *pmb = rc->pmy_block;
                pmb->SetBlockTimestep(parthenon::Update::EstimateTimestep(rc));
                return TaskStatus::complete;
            }, t_fill_derived, sc1);

        // Update refinement
        if (pmesh->adaptive) {
            auto tag_refine = tl.AddTask([](MeshBlock *pmb) {
                pmb->pmr->CheckRefinementCondition();
                return TaskStatus::complete;
            }, t_fill_derived, pmb);
        }
    }
    return tl;
}