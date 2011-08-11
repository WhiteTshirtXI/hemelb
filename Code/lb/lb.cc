// In this file, the functions useful to calculate the equilibrium distribution
// function, momentums, the effective von Mises stress and the boundary conditions
// are reported

#include <math.h>
#include <limits>

#include "lb/lb.h"
#include "util/utilityFunctions.h"
#include "vis/RayTracer.h"

namespace hemelb
{
  namespace lb
  {
    void LBM::RecalculateTauViscosityOmega()
    {
      mParams.Tau = 0.5 + (PULSATILE_PERIOD_s * BLOOD_VISCOSITY_Pa_s / BLOOD_DENSITY_Kg_per_m3)
          / (Cs2 * ((double) mState->GetTimeStepsPerCycle() * voxel_size * voxel_size));

      mParams.Omega = -1.0 / mParams.Tau;
      mParams.StressParameter = (1.0 - 1.0 / (2.0 * mParams.Tau)) / sqrt(2.0);
      mParams.Beta = -1.0 / (2.0 * mParams.Tau);
    }

    hemelb::lb::LbmParameters *LBM::GetLbmParams()
    {
      return &mParams;
    }

    LBM::LBM(SimConfig *iSimulationConfig,
             net::Net* net,
             geometry::LatticeData* latDat,
             SimulationState* simState) :
      mSimConfig(iSimulationConfig), mNet(net), mLatDat(latDat), mState(simState)
    {
      voxel_size = iSimulationConfig->VoxelSize;

      ReadParameters();
    }

    void LBM::CalculateMouseFlowField(float densityIn,
                                      float stressIn,
                                      distribn_t &mouse_pressure,
                                      distribn_t &mouse_stress,
                                      double density_threshold_min,
                                      double density_threshold_minmax_inv,
                                      double stress_threshold_max_inv)
    {
      double density = density_threshold_min + densityIn / density_threshold_minmax_inv;
      double stress = stressIn / stress_threshold_max_inv;

      mouse_pressure = mUnits->ConvertPressureToPhysicalUnits(density * Cs2);
      mouse_stress = mUnits->ConvertStressToPhysicalUnits(stress);
    }

    template<typename tMidFluidCollision, typename tWallCollision, typename tInletOutletCollision,
        typename tInletOutletWallCollision, typename tCollisionOperator>
    void LBM::InitCollisions(BoundaryComms* iBoundaryComms)
    {
      mStreamAndCollide
          = new hemelb::lb::collisions::StreamAndCollide<tMidFluidCollision, tWallCollision,
              tInletOutletCollision, tInletOutletWallCollision, tCollisionOperator>(mCollisionOperator);
      mPostStep = new hemelb::lb::collisions::PostStep<tMidFluidCollision, tWallCollision,
          tInletOutletCollision, tInletOutletWallCollision>();

      // TODO Note that the convergence checking is not yet implemented in the
      // new boundary condition hierarchy system.
      // It'd be nice to do this with something like
      // MidFluidCollision = new ConvergenceCheckingWrapper(new WhateverMidFluidCollision());

      mMidFluidCollision = new hemelb::lb::collisions::MidFluidCollision();
      mWallCollision = new hemelb::lb::collisions::WallCollision();
      mInletCollision
          = new hemelb::lb::collisions::InletOutletCollision(iBoundaryComms, INLET);
      mOutletCollision
          = new hemelb::lb::collisions::InletOutletCollision(iBoundaryComms, OUTLET);
      mInletWallCollision
          = new hemelb::lb::collisions::InletOutletWallCollision(iBoundaryComms, INLET);
      mOutletWallCollision
          = new hemelb::lb::collisions::InletOutletWallCollision(iBoundaryComms, OUTLET);
    }

    void LBM::Initialise(site_t* iFTranslator,
                         vis::Control* iControl,
                         BoundaryComms* iBoundaryComms,
                         util::UnitConverter* iUnits)
    {
      mUnits = iUnits;

      outlet_density_avg = iBoundaryComms->outlet_density_avg;
      outlet_density_amp = iBoundaryComms->outlet_density_amp;
      inlet_density_avg = iBoundaryComms->inlet_density_avg;
      inlet_density_amp = iBoundaryComms->inlet_density_amp;

      mCollisionOperator = new CO(mLatDat, &mParams);

      InitCollisions<hemelb::lb::collisions::implementations::SimpleCollideAndStream<CO>,
          hemelb::lb::collisions::implementations::ZeroVelocityEquilibrium<CO>,
          hemelb::lb::collisions::implementations::NonZeroVelocityBoundaryDensity<CO>,
          hemelb::lb::collisions::implementations::ZeroVelocityBoundaryDensity<CO>, CO> (iBoundaryComms);

      receivedFTranslator = iFTranslator;

      SetInitialConditions();

      mVisControl = iControl;
    }

    void LBM::SetInitialConditions()
    {
      distribn_t *f_old_p, *f_new_p, f_eq[D3Q15::NUMVECTORS];
      distribn_t density;

      density = 0.;

      for (int i = 0; i < outlets; i++)
      {
        density += outlet_density_avg[i] - outlet_density_amp[i];
      }
      density /= outlets;

      for (site_t i = 0; i < mLatDat->GetLocalFluidSiteCount(); i++)
      {
        D3Q15::CalculateFeq(density, 0.0, 0.0, 0.0, f_eq);

        f_old_p = mLatDat->GetFOld(i * D3Q15::NUMVECTORS);
        f_new_p = mLatDat->GetFNew(i * D3Q15::NUMVECTORS);

        for (unsigned int l = 0; l < D3Q15::NUMVECTORS; l++)
        {
          f_new_p[l] = f_old_p[l] = f_eq[l];
        }
      }
    }

    // TODO HACK
    hemelb::lb::collisions::Collision* LBM::GetCollision(int i)
    {
      switch (i)
      {
        case 0:
          return mMidFluidCollision;
        case 1:
          return mWallCollision;
        case 2:
          return mInletCollision;
        case 3:
          return mOutletCollision;
        case 4:
          return mInletWallCollision;
        case 5:
          return mOutletWallCollision;
      }
      return NULL;
    }

    void LBM::RequestComms()
    {
      topology::NetworkTopology* netTop = topology::NetworkTopology::Instance();

      for (std::vector<hemelb::topology::NeighbouringProcessor>::const_iterator it =
          netTop->NeighbouringProcs.begin(); it != netTop->NeighbouringProcs.end(); it++)
      {
        // Request the receive into the appropriate bit of FOld.
        mNet->RequestReceive<distribn_t> (mLatDat->GetFOld( (*it).FirstSharedF),
                                          (int) (*it).SharedFCount,
                                           (*it).Rank);

        // Request the send from the right bit of FNew.
        mNet->RequestSend<distribn_t> (mLatDat->GetFNew( (*it).FirstSharedF),
                                       (int) (*it).SharedFCount,
                                        (*it).Rank);

      }
    }

    void LBM::PreSend()
    {
      site_t offset = mLatDat->GetInnerSiteCount();

      for (unsigned int collision_type = 0; collision_type < COLLISION_TYPES; collision_type++)
      {
        GetCollision(collision_type)->AcceptCollisionVisitor(mStreamAndCollide,
                                                             mVisControl->IsRendering(),
                                                             offset,
                                                             mLatDat->GetInterCollisionCount(collision_type),
                                                             &mParams,
                                                             mLatDat,
                                                             mVisControl);
        offset += mLatDat->GetInterCollisionCount(collision_type);
      }
    }

    void LBM::PreReceive()
    {
      site_t offset = 0;

      for (unsigned int collision_type = 0; collision_type < COLLISION_TYPES; collision_type++)
      {
        GetCollision(collision_type)->AcceptCollisionVisitor(mStreamAndCollide,
                                                             mVisControl->IsRendering(),
                                                             offset,
                                                             mLatDat->GetInnerCollisionCount(collision_type),
                                                             &mParams,
                                                             mLatDat,
                                                             mVisControl);
        offset += mLatDat->GetInnerCollisionCount(collision_type);
      }
    }

    void LBM::PostReceive()
    {
      // Copy the distribution functions received from the neighbouring
      // processors into the destination buffer "f_new".
      topology::NetworkTopology* netTop = topology::NetworkTopology::Instance();

      for (site_t i = 0; i < netTop->TotalSharedFs; i++)
      {
        *mLatDat->GetFNew(receivedFTranslator[i])
            = *mLatDat->GetFOld(netTop->NeighbouringProcs[0].FirstSharedF + i);
      }

      // Do any cleanup steps necessary on boundary nodes
      size_t offset = 0;

      for (unsigned int collision_type = 0; collision_type < COLLISION_TYPES; collision_type++)
      {
        GetCollision(collision_type)->AcceptCollisionVisitor(mPostStep,
                                                             mVisControl->IsRendering(),
                                                             offset,
                                                             mLatDat->GetInnerCollisionCount(collision_type),
                                                             &mParams,
                                                             mLatDat,
                                                             mVisControl);
        offset += mLatDat->GetInnerCollisionCount(collision_type);
      }

      for (unsigned int collision_type = 0; collision_type < COLLISION_TYPES; collision_type++)
      {
        GetCollision(collision_type)->AcceptCollisionVisitor(mPostStep,
                                                             mVisControl->IsRendering(),
                                                             offset,
                                                             mLatDat->GetInterCollisionCount(collision_type),
                                                             &mParams,
                                                             mLatDat,
                                                             mVisControl);
        offset += mLatDat->GetInterCollisionCount(collision_type);
      }
    }

    void LBM::EndIteration()
    {
      // Swap f_old and f_new ready for the next timestep.
      mLatDat->SwapOldAndNew();
    }

    // Update peak and average inlet velocities local to the current subdomain.
    void LBM::UpdateInletVelocities(unsigned long time_step)
    {
      distribn_t density;
      distribn_t vx, vy, vz;
      distribn_t velocity;

      int inlet_id;

      site_t offset = mLatDat->GetInnerCollisionCount(0) + mLatDat->GetInnerCollisionCount(1);

      for (site_t i = offset; i < offset + mLatDat->GetInnerCollisionCount(2); i++)
      {
        D3Q15::CalculateDensityAndVelocity(mLatDat->GetFOld(i * D3Q15::NUMVECTORS),
                                           density,
                                           vx,
                                           vy,
                                           vz);

        inlet_id = mLatDat->GetBoundaryId(i);

        vx *= inlet_normal[3 * inlet_id + 0];
        vy *= inlet_normal[3 * inlet_id + 1];
        vz *= inlet_normal[3 * inlet_id + 2];

        velocity = vx * vx + vy * vy + vz * vz;

        if (velocity > 0.)
        {
          velocity = sqrt(velocity) / density;
        }
        else
        {
          velocity = -sqrt(velocity) / density;
        }
      }

      offset = mLatDat->GetInnerSiteCount() + mLatDat->GetInterCollisionCount(0)
          + mLatDat->GetInterCollisionCount(1);

      for (site_t i = offset; i < offset + mLatDat->GetInterCollisionCount(2); i++)
      {
        D3Q15::CalculateDensityAndVelocity(mLatDat->GetFOld(i * D3Q15::NUMVECTORS),
                                           density,
                                           vx,
                                           vy,
                                           vz);

        inlet_id = mLatDat->GetBoundaryId(i);

        vx *= inlet_normal[3 * inlet_id + 0];
        vy *= inlet_normal[3 * inlet_id + 1];
        vz *= inlet_normal[3 * inlet_id + 2];

        velocity = vx * vx + vy * vy + vz * vz;

        if (velocity > 0.)
        {
          velocity = sqrt(velocity) / density;
        }
        else
        {
          velocity = -sqrt(velocity) / density;
        }
      }
    }

    // In the case of instability, this function restart the simulation
    // with twice as many time steps per period and update the parameters
    // that depends on this change.
    void LBM::Reset()
    {
      RecalculateTauViscosityOmega();

      SetInitialConditions();

      mCollisionOperator->Reset(mLatDat, &mParams);
    }

    LBM::~LBM()
    {
      // Delete the translator between received location and location in f_new.
      delete[] receivedFTranslator;

      // Delete visitors
      delete mStreamAndCollide;
      delete mPostStep;

      // Delete Collision Operator
      delete mCollisionOperator;

      // Delete the collision and stream objects we've been using
      delete mMidFluidCollision;
      delete mWallCollision;
      delete mInletCollision;
      delete mOutletCollision;
      delete mInletWallCollision;
      delete mOutletWallCollision;

      // Delete various other arrays used
      delete[] inlet_normal;
    }
  }
}
