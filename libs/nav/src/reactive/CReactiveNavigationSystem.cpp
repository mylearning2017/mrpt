/* +---------------------------------------------------------------------------+
   |                     Mobile Robot Programming Toolkit (MRPT)               |
   |                          http://www.mrpt.org/                             |
   |                                                                           |
   | Copyright (c) 2005-2017, Individual contributors, see AUTHORS file        |
   | See: http://www.mrpt.org/Authors - All rights reserved.                   |
   | Released under BSD License. See details in http://www.mrpt.org/License    |
   +---------------------------------------------------------------------------+ */

#include "nav-precomp.h" // Precomp header

#include <mrpt/nav/reactive/CReactiveNavigationSystem.h>
#include <mrpt/nav/tpspace/CPTG_DiffDrive_CollisionGridBased.h>
#include <mrpt/system/filesystem.h>
#include <mrpt/utils/CConfigFileMemory.h>

using namespace mrpt;
using namespace mrpt::poses;
using namespace mrpt::math;
using namespace mrpt::utils;
using namespace mrpt::nav;
using namespace std;


/*---------------------------------------------------------------
					Constructor
  ---------------------------------------------------------------*/
CReactiveNavigationSystem::CReactiveNavigationSystem(
	CRobot2NavInterface   &react_iterf_impl,
	bool					enableConsoleOutput,
	bool					enableLogToFile,
	const std::string &logFileDirectory
)
	:
	CAbstractPTGBasedReactive(react_iterf_impl,enableConsoleOutput,enableLogToFile, logFileDirectory)
{
}

// Dtor:
CReactiveNavigationSystem::~CReactiveNavigationSystem()
{
	this->preDestructor();

	// Free PTGs:
	for (size_t i=0;i<PTGs.size();i++)	delete PTGs[i];
	PTGs.clear();
}


/*---------------------------------------------------------------
						changeRobotShape
  ---------------------------------------------------------------*/
void CReactiveNavigationSystem::changeRobotShape( const math::CPolygon &shape )
{
	m_PTGsMustBeReInitialized = true;
	if (shape.verticesCount() < 3) {
		THROW_EXCEPTION("The robot shape has less than 3 vertices!!")
	}
	m_robotShape = shape;
}
void CReactiveNavigationSystem::changeRobotCircularShapeRadius( const double R )
{
	m_PTGsMustBeReInitialized = true;
	ASSERT_(R>0);
	m_robotShapeCircularRadius = R;
}


void CReactiveNavigationSystem::saveConfigFile(mrpt::utils::CConfigFileBase &c) const
{
	CAbstractPTGBasedReactive::saveConfigFile(c);

	const std::string s = "CReactiveNavigationSystem";
	params_reactive_nav.saveToConfigFile(c, s);

	unsigned int PTG_COUNT = PTGs.size();
	MRPT_SAVE_CONFIG_VAR_COMMENT(PTG_COUNT, "Number of PTGs");
}

void CReactiveNavigationSystem::loadConfigFile(const mrpt::utils::CConfigFileBase &c)
{
	MRPT_START

	// 1st: load my own params; at the end, call parent's overriden method:
	const std::string sectCfg = "CReactiveNavigationSystem";
	this->params_reactive_nav.loadFromConfigFile(c, sectCfg);

	unsigned int PTG_COUNT = c.read_int(sectCfg,"PTG_COUNT",0, true );

	// Load robot shape: 1/2 polygon
	// ---------------------------------------------
	vector<float>        xs,ys;
	c.read_vector(sectCfg,"RobotModel_shape2D_xs",vector<float>(0), xs, false );
	c.read_vector(sectCfg,"RobotModel_shape2D_ys",vector<float>(0), ys, false );
	ASSERTMSG_(xs.size()==ys.size(),"Config parameters `RobotModel_shape2D_xs` and `RobotModel_shape2D_ys` must have the same length!");
	if (!xs.empty())
	{
		math::CPolygon shape;
		for (size_t i=0;i<xs.size();i++)
			shape.AddVertex(xs[i],ys[i]);
		changeRobotShape( shape );
	}

	// Load robot shape: 2/2 circle
	// ---------------------------------------------
	const double robot_shape_radius = c.read_double(sectCfg,"RobotModel_circular_shape_radius",.0, false );
	ASSERT_(robot_shape_radius>=.0);
	if (robot_shape_radius!=.0)
	{
		changeRobotCircularShapeRadius( robot_shape_radius );
	}

	// Load PTGs from file:
	// ---------------------------------------------
	// Free previous PTGs:
	for (size_t i=0;i<PTGs.size();i++)	delete PTGs[i];
	PTGs.assign(PTG_COUNT,NULL);

	for ( unsigned int n=0;n<PTG_COUNT;n++)
	{
		// Factory:
		const std::string sPTGName = c.read_string(sectCfg,format("PTG%u_Type", n ),"", true );
		PTGs[n] = CParameterizedTrajectoryGenerator::CreatePTG(sPTGName,c,sectCfg, format("PTG%u_",n) );
	}


	CAbstractPTGBasedReactive::loadConfigFile(c); // call parent's overriden method:

	MRPT_END
}

void CReactiveNavigationSystem::STEP1_InitPTGs()
{
	if (m_PTGsMustBeReInitialized)
	{
		m_PTGsMustBeReInitialized = false;

		mrpt::utils::CTimeLoggerEntry tle(m_timelogger,"STEP1_InitPTGs");

		for (unsigned int i=0;i<PTGs.size();i++)
		{
			PTGs[i]->deinitialize();

			logFmt(mrpt::utils::LVL_INFO,"[CReactiveNavigationSystem::STEP1_InitPTGs] Initializing PTG#%u (`%s`)...", i,PTGs[i]->getDescription().c_str());

			// Polygonal robot shape?
			{
				mrpt::nav::CPTG_RobotShape_Polygonal *ptg = dynamic_cast<mrpt::nav::CPTG_RobotShape_Polygonal *>(PTGs[i]);
				if (ptg)
					ptg->setRobotShape(m_robotShape);
			}
			// Circular robot shape?
			{
				mrpt::nav::CPTG_RobotShape_Circular *ptg = dynamic_cast<mrpt::nav::CPTG_RobotShape_Circular*>(PTGs[i]);
				if (ptg)
					ptg->setRobotShapeRadius(m_robotShapeCircularRadius);
			}

			// Init:
			PTGs[i]->initialize(
				format("%s/ReacNavGrid_%03u.dat.gz", params_abstract_ptg_navigator.ptg_cache_files_directory.c_str(), i),
				m_enableConsoleOutput /*verbose*/
			);
			logStr(mrpt::utils::LVL_INFO,"Done!");
		}
	}
}

bool CReactiveNavigationSystem::implementSenseObstacles(mrpt::system::TTimeStamp &obstacles_timestamp)
{
	try
	{
		bool ret; // Return true on success
		{
			CTimeLoggerEntry tle1(m_timelogger, "navigationStep.STEP2_Sense");
			CTimeLoggerEntry tle2(m_timlog_delays, "senseObstacles()");
			ret = m_robot.senseObstacles(m_WS_Obstacles, obstacles_timestamp);
		}

		// Optional filtering of obstacles:
		m_WS_Obstacles_original = m_WS_Obstacles;
		if (ret && m_WS_filter.present())
		{
			m_WS_filter->filter(&m_WS_Obstacles, obstacles_timestamp, mrpt::poses::CPose3D(mrpt::math::TPose3D(m_curPoseVel.pose)) );
		}

		return ret;
		// Note: Clip obstacles by "z" axis coordinates is more efficiently done in STEP3_WSpaceToTPSpace()
	}
	catch (std::exception &e)
	{
		MRPT_LOG_ERROR_STREAM( "[CReactiveNavigationSystem::STEP2_Sense] Exception:" << e.what());
		return false;
	}
	catch (...)
	{
		MRPT_LOG_ERROR_STREAM( "[CReactiveNavigationSystem::STEP2_Sense] Unexpected exception!");
		return false;
	}

}

void CReactiveNavigationSystem::STEP3_WSpaceToTPSpace(const size_t ptg_idx,std::vector<double> &out_TPObstacles, mrpt::nav::ClearanceDiagram &out_clearance, const mrpt::math::TPose2D &rel_pose_PTG_origin_wrt_sense_, const bool eval_clearance)
{
	CParameterizedTrajectoryGenerator	*ptg = this->getPTG(ptg_idx);

	const mrpt::poses::CPose2D rel_pose_PTG_origin_wrt_sense(rel_pose_PTG_origin_wrt_sense_);

	const float OBS_MAX_XY = params_abstract_ptg_navigator.ref_distance*1.1f;

	// Merge all the (k,d) for which the robot collides with each obstacle point:
	size_t nObs;
	const float *xs,*ys,*zs;
	m_WS_Obstacles.getPointsBuffer(nObs,xs,ys,zs);

	for (size_t obs=0;obs<nObs;obs++)
	{
		double ox,oy,oz=zs[obs];
		rel_pose_PTG_origin_wrt_sense.composePoint(xs[obs], ys[obs], ox, oy);

		if (ox>-OBS_MAX_XY && ox<OBS_MAX_XY &&
			oy>-OBS_MAX_XY && oy<OBS_MAX_XY &&
			oz>=params_reactive_nav.min_obstacles_height && oz<= params_reactive_nav.max_obstacles_height)
		{
			ptg->updateTPObstacle(ox, oy, out_TPObstacles);
			if (eval_clearance) {
				ptg->updateClearance(ox, oy, out_clearance);
			}
		}
	}
}


/** Generates a pointcloud of obstacles, and the robot shape, to be saved in the logging record for the current timestep */
void CReactiveNavigationSystem::loggingGetWSObstaclesAndShape(CLogFileRecord &out_log)
{
	out_log.WS_Obstacles = m_WS_Obstacles;
	out_log.WS_Obstacles_original = m_WS_Obstacles_original;

	const size_t nVerts = m_robotShape.size();
	out_log.robotShape_x.resize(nVerts);
	out_log.robotShape_y.resize(nVerts);
	out_log.robotShape_radius = m_robotShapeCircularRadius;

	for (size_t i=0;i<nVerts;i++)
	{
		out_log.robotShape_x[i]= m_robotShape.GetVertex_x(i);
		out_log.robotShape_y[i]= m_robotShape.GetVertex_y(i);
	}
}

void CReactiveNavigationSystem::TReactiveNavigatorParams::loadFromConfigFile(const mrpt::utils::CConfigFileBase & c, const std::string & s)
{
	MRPT_LOAD_CONFIG_VAR_REQUIRED_CS(min_obstacles_height, double);
	MRPT_LOAD_CONFIG_VAR_REQUIRED_CS(max_obstacles_height, double);
}

void CReactiveNavigationSystem::TReactiveNavigatorParams::saveToConfigFile(mrpt::utils::CConfigFileBase & c, const std::string & s) const
{
	MRPT_SAVE_CONFIG_VAR_COMMENT(min_obstacles_height, "Minimum `z` coordinate of obstacles to be considered fo collision checking");
	MRPT_SAVE_CONFIG_VAR_COMMENT(max_obstacles_height, "Maximum `z` coordinate of obstacles to be considered fo collision checking");
}

CReactiveNavigationSystem::TReactiveNavigatorParams::TReactiveNavigatorParams() :
	min_obstacles_height(0.0),
	max_obstacles_height(10.0)
{
}
