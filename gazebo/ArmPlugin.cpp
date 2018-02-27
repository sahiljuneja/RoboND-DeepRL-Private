/*
 * http://github.com/dusty-nv/jetson-reinforcement
 */

#include "ArmPlugin.h"
#include "PropPlugin.h"

#include "cudaMappedMemory.h"
#include "cudaPlanar.h"


#define PI 3.141592653589793238462643383279502884197169f

#define JOINT_MIN	-0.75f
#define JOINT_MAX	 2.0f

#define VELOCITY_CONTROL
#define VELOCITY_MIN -0.2f
#define VELOCITY_MAX  0.2f

#define INPUT_WIDTH   128
#define INPUT_HEIGHT  128
#define INPUT_CHANNELS 3

#define WORLD_NAME "arm_world"
#define PROP_NAME  "box"
#define GRIP_NAME  "gripperright"

#define REWARD_WIN  1000.0f
#define REWARD_LOSS -1000.0f

#define COLLISION_FILTER "ground_plane::link::collision"

#define ANIMATION_STEPS 2000


namespace gazebo
{
 
// register this plugin with the simulator
GZ_REGISTER_MODEL_PLUGIN(ArmPlugin)


// constructor
ArmPlugin::ArmPlugin() : ModelPlugin(), cameraNode(new gazebo::transport::Node()), collisionNode(new gazebo::transport::Node())
{
	printf("ArmPlugin::ArmPlugin()\n");

	for( uint32_t n=0; n < DOF; n++ )
		resetPos[n] = 0.0f;

	//resetPos[1] = -0.25;
	//resetPos[2] = 1.5;
	resetPos[3] = 0.25;    // custom reset positions

	for( uint32_t n=0; n < DOF; n++ )
	{
		ref[n] = resetPos[n]; //JOINT_MIN;
		vel[n] = 0.0f;
	}

	agent 	       = NULL;
	inputState       = NULL;
	inputBuffer[0]   = NULL;
	inputBuffer[1]   = NULL;
	inputBufferSize  = 0;
	inputRawWidth    = 0;
	inputRawHeight   = 0;
	actionJointDelta = 0.15f;
	actionVelDelta   = 0.1f;
	maxEpisodeLength = 100;
	episodeFrames    = 0;

	newState         = false;
	newReward        = false;
	endEpisode       = false;
	rewardHistory    = 0.0f;
	testAnimation    = false;
	loopAnimation    = false;
	animationStep    = 0;
	lastGoalDistance = 0.0f;
	avgGoalDelta     = 0.0f;
}


// Load
void ArmPlugin::Load(physics::ModelPtr _parent, sdf::ElementPtr /*_sdf*/) 
{
	printf("ArmPlugin::Load('%s')\n", _parent->GetName().c_str());

	// Create AI agent
	agent = dqnAgent::Create(INPUT_WIDTH, INPUT_HEIGHT, INPUT_CHANNELS, DOF*2 + 1/*+1*/);

	if( !agent )
		printf("ArmPlugin - failed to create AI agent\n");

	inputState = Tensor::Alloc(INPUT_WIDTH, INPUT_HEIGHT, INPUT_CHANNELS);

	if( !inputState )
		printf("ArmPlugin - failed to allocate %ux%ux%u Tensor\n", INPUT_WIDTH, INPUT_HEIGHT, INPUT_CHANNELS);

	// Store the pointer to the model
	this->model = _parent;
	this->j2_controller = new physics::JointController(model);

	// Create our node for camera communication
	cameraNode->Init();
	cameraSub = cameraNode->Subscribe("/gazebo/" WORLD_NAME "/camera/link/camera/image", &ArmPlugin::onCameraMsg, this);

	// Create our node for collision detection
	collisionNode->Init();
	collisionSub = collisionNode->Subscribe("/gazebo/" WORLD_NAME "/" PROP_NAME "/link/my_contact", &ArmPlugin::onCollisionMsg, this);

	// Listen to the update event. This event is broadcast every simulation iteration.
	this->updateConnection = event::Events::ConnectWorldUpdateBegin(boost::bind(&ArmPlugin::OnUpdate, this, _1));
}


// onCameraMsg
void ArmPlugin::onCameraMsg(ConstImageStampedPtr &_msg)
{
	// check the validity of the message contents
	if( !_msg )
	{
		printf("ArmPlugin - recieved NULL message\n");
		return;
	}

	// retrieve image dimensions
	const int width  = _msg->image().width();
	const int height = _msg->image().height();
	const int bpp    = (_msg->image().step() / _msg->image().width()) * 8;	// bits per pixel
	const int size   = _msg->image().data().size();

	if( bpp != 24 )
	{
		printf("ArmPlugin - expected 24BPP uchar3 image from camera, got %i\n", bpp);
		return;
	}

	// allocate temp image if necessary
	if( !inputBuffer[0] || size != inputBufferSize )
	{
		if( !cudaAllocMapped(&inputBuffer[0], &inputBuffer[1], size) )
		{
			printf("ArmPlugin - cudaAllocMapped() failed to allocate %i bytes\n", size);
			return;
		}

		printf("ArmPlugin - allocated camera img buffer %ix%i  %i bpp  %i bytes\n", width, height, bpp, size);
		
		inputBufferSize = size;
		inputRawWidth   = width;
		inputRawHeight  = height;
	}

	memcpy(inputBuffer[0], _msg->image().data().c_str(), inputBufferSize);
	newState = true;

	/* unsigned int oldCount = this->data.image_count;
	this->data.image_count = _msg->image().data().size();

	if (oldCount != this->data.image_count)
	{
		delete this->data.image;
		this->data.image = new uint8_t[this->data.image_count];
	}

	// Set the image pixels
	memcpy(this->data.image, _msg->image().data().c_str(),_msg->image().data().size());

	size = sizeof(this->data) - sizeof(this->data.image) +
	_msg->image().data().size(); */

	printf("camera %i x %i  %i bpp  %i bytes\n", width, height, bpp, size);
	//std::cout << _msg->DebugString();
}


// onCollisionMsg
void ArmPlugin::onCollisionMsg(ConstContactsPtr &contacts)
{
	//printf("collision callback (%u contacts)\n", contacts->contact_size());

	for (unsigned int i = 0; i < contacts->contact_size(); ++i)
	{
		if( strcmp(contacts->contact(i).collision2().c_str(), COLLISION_FILTER) == 0 )
			continue;

		std::cout << "Collision between[" << contacts->contact(i).collision1()
			     << "] and [" << contacts->contact(i).collision2() << "]\n";

		for (unsigned int j = 0; j < contacts->contact(i).position_size(); ++j)
		{
			 std::cout << j << "  Position:"
					 << contacts->contact(i).position(j).x() << " "
					 << contacts->contact(i).position(j).y() << " "
					 << contacts->contact(i).position(j).z() << "\n";
			 std::cout << "   Normal:"
					 << contacts->contact(i).normal(j).x() << " "
					 << contacts->contact(i).normal(j).y() << " "
					 << contacts->contact(i).normal(j).z() << "\n";
			 std::cout << "   Depth:" << contacts->contact(i).depth(j) << "\n";
		}

		// issue learning reward
		if( !testAnimation )
		{
			//rewardHistory = (1.0f - (float(episodeFrames) / float(maxEpisodeLength))) * REWARD_WIN;
			rewardHistory = REWARD_WIN;

			newReward  = true;
			endEpisode = true;
		}
	}
}


// upon recieving a new frame, update the AI agent
bool ArmPlugin::updateAgent()
{
	// convert uchar3 input from camera to planar BGR
	if( CUDA_FAILED(cudaPackedToPlanarBGR((uchar3*)inputBuffer[1], inputRawWidth, inputRawHeight,
							         inputState->gpuPtr, INPUT_WIDTH, INPUT_HEIGHT)) )
	{
		printf("ArmPlugin - failed to convert %zux%zu image to %ux%u planar BGR image\n",
			   inputRawWidth, inputRawHeight, INPUT_WIDTH, INPUT_HEIGHT);

		return false;
	}

	// select the next action
	int action = 0;

	if( !agent->NextAction(inputState, &action) )
	{
		printf("ArmPlugin - failed to generate agent's next action\n");
		return false;
	}

	// make sure the selected action is in-bounds
	if( action < 0 || action >= DOF * 2 + 1 )
	{
		printf("ArmPlugin - agent selected invalid action, %i\n", action);
		return false;
	}

	printf("ArmPlugin - agent selected action %i\n", action);

	// action 0 does nothing, the others index a joint
	/*if( action == 0 )
		return false;	// not an error, but didn't cause an update
	
	action--;*/	// with action 0 = no-op, index 1 should map to joint 0

	// if the action is even, increase the joint position by the delta parameter
	// if the action is odd,  decrease the joint position by the delta parameter
#ifdef VELOCITY_CONTROL
	float velocity = vel[action/2] + actionVelDelta * ((action % 2 == 0) ? 1.0f : -1.0f);

	if( velocity < VELOCITY_MIN )
		velocity = VELOCITY_MIN;

	if( velocity > VELOCITY_MAX )
		velocity = VELOCITY_MAX;

	vel[action/2] = velocity;
	
	for( uint32_t n=0; n < DOF; n++ )
	{
		ref[n] += vel[n];

		if( ref[n] < JOINT_MIN )
		{
			ref[n] = JOINT_MIN;
			vel[n] = 0.0f;
		}
		else if( ref[n] > JOINT_MAX )
		{
			ref[n] = JOINT_MAX;
			vel[n] = 0.0f;
		}
	}
#else
	float joint = ref[action/2] + actionJointDelta * ((action % 2 == 0) ? 1.0f : -1.0f);

	// limit the joint to the specified range
	if( joint < JOINT_MIN )
		joint = JOINT_MIN;
	
	if( joint > JOINT_MAX )
		joint = JOINT_MAX;

	ref[action/2] = joint;
#endif

	return true;
}


// update joint reference positions, returns true if positions have been modified
bool ArmPlugin::updateJoints()
{
	if( testAnimation )	// test sequence
	{
		const float step = (JOINT_MAX - JOINT_MIN) * (float(1.0f) / float(ANIMATION_STEPS));
#if 0
		// range of motion
		if( animationStep < ANIMATION_STEPS )
		{
			//for( uint32_t n=0; n < DOF; n++ )
			/*ref[0] += dT[0];
			ref[1] += dT[1];	//ref[4] += dT[1];
			ref[2] += dT[2];	//ref[8] += dT[2];*/

			animationStep++;
			printf("animation step %u\n", animationStep);

			for( uint32_t n=0; n < DOF; n++ )
				ref[n] = JOINT_MIN + step * float(animationStep);
		}
		else if( animationStep < ANIMATION_STEPS * 2 )
		{
			/*ref[0] -= dT[0];
			ref[1] -= dT[1];
			ref[2] -= dT[2];*/

			animationStep++;
			printf("animation step %u\n", animationStep);

			for( uint32_t n=0; n < DOF; n++ )
				ref[n] = JOINT_MAX - step * float(animationStep-ANIMATION_STEPS);
		}
		else
		{
			animationStep = 0;

			//const float r = float(rand()) / float(RAND_MAX);
			//setAnimationTarget( 10000.0f, 0.0f );
		}
#else
		// return to base position
		for( uint32_t n=0; n < DOF; n++ )
		{
			/*float diff = ref[n] - resetPos[n];

			if( diff < 0.0f )
				diff = -diff;

			if( diff < step )
				step = diff;*/

			if( ref[n] < resetPos[n] )
				ref[n] += step;
			else if( ref[n] > resetPos[n] )
				ref[n] -= step;

			if( ref[n] < JOINT_MIN )
				ref[n] = JOINT_MIN;
			else if( ref[n] > JOINT_MAX )
				ref[n] = JOINT_MAX;
		}

		animationStep++;
#endif

		// reset and loop the animation
		if( animationStep > ANIMATION_STEPS )
		{
			animationStep = 0;
			
			if( !loopAnimation )
				testAnimation = false;
		}
		else if( animationStep == ANIMATION_STEPS / 2 )
			RandomizeProps();
			//ResetPropDynamics();

		return true;
	}
	else if( newState && agent != NULL )
	{
		// update the AI agent when new camera frame is ready
		episodeFrames++;
		printf("episode frame = %i\n", episodeFrames);

		// reset camera ready flag
		newState = false;

		if( updateAgent() )
			return true;
	}

	return false;
}


// get the servo center for a particular degree of freedom
float ArmPlugin::resetPosition( uint32_t dof )
{
	return resetPos[dof];
}


// compute the distance between two bounding boxes
static float BoxDistance(const math::Box& a, const math::Box& b)
{
	float sqrDist = 0;

	if( b.max.x < a.min.x )
	{
		float d = b.max.x - a.min.x;
		sqrDist += d * d;
	}
	else if( b.min.x > a.max.x )
	{
		float d = b.min.x - a.max.x;
		sqrDist += d * d;
	}

	if( b.max.y < a.min.y )
	{
		float d = b.max.y - a.min.y;
		sqrDist += d * d;
	}
	else if( b.min.y > a.max.y )
	{
		float d = b.min.y - a.max.y;
		sqrDist += d * d;
	}

	if( b.max.z < a.min.z )
	{
		float d = b.max.z - a.min.z;
		sqrDist += d * d;
	}
	else if( b.min.z > a.max.z )
	{
		float d = b.min.z - a.max.z;
		sqrDist += d * d;
	}
	
	return sqrtf(sqrDist);
}

// compute the distance between two bounding boxes
static bool isPropInside (const math::Box& a, const math::Box& b, const math::Box& c)
{
	float sqrDist = 0;

	if( a.min.x < c.min.x && b.min.x < c.min.x && a.max.y < c.min.y && b.min.y > c.max.y && a.min.z > c.min.z && b.min.z > c.min.z && a.max.z < c.max.z && b.max.z < c.max.z)
	{
		return true;
    }
    else
    {
        return false;
    }
}


// called by the world update start event
void ArmPlugin::OnUpdate(const common::UpdateInfo & /*_info*/)
{
   /*const math::Pose& pose = model->GetWorldPose();
	printf("%s location:  %lf %lf %lf\n", model->GetName().c_str(), pose.pos.x, pose.pos.y, pose.pos.z);
	
	const math::Box& bbox = model->GetBoundingBox();
	printf("%s bounding:  min=%lf %lf %lf  max=%lf %lf %lf\n", model->GetName().c_str(), bbox.min.x, bbox.min.y, bbox.min.z,bbox.max.x, bbox.max.y, bbox.max.z);
   */
   /*const math::Vector3 center = bbox.GetCenter();
	const math::Vector3 bbSize = bbox.GetSize();

	printf("arm bounding:  center=%lf %lf %lf  size=%lf %lf %lf\n", center.x, center.y, center.z, bbSize.x, bbSize.y, bbSize.z); */
	const bool hadNewState = newState && !testAnimation;

	// update the robot positions with vision/DQN
	if( updateJoints() )
	{
		//printf("%f  %f  %f  %s\n", ref[0], ref[1], ref[2], testAnimation ? "(testAnimation)" : "(agent)");

		double angle(1);
		//std::string j2name("joint1");  
		j2_controller->SetJointPosition(this->model->GetJoint("base"), 	 ref[0]); 
		j2_controller->SetJointPosition(this->model->GetJoint("joint1"),  ref[1]);
		j2_controller->SetJointPosition(this->model->GetJoint("joint2"),  ref[2]);
		j2_controller->SetJointPosition(this->model->GetJoint("joint3"),  ref[3]);
		
		/*j2_controller->SetJointPosition(this->model->GetJoint("joint4"),  ref[3]);
		j2_controller->SetJointPosition(this->model->GetJoint("joint5"),  ref[4]);
		j2_controller->SetJointPosition(this->model->GetJoint("joint6"),  ref[5]);
		j2_controller->SetJointPosition(this->model->GetJoint("joint7"),  ref[6]);
		j2_controller->SetJointPosition(this->model->GetJoint("joint8"),  ref[7]);
		j2_controller->SetJointPosition(this->model->GetJoint("joint9"),  ref[8]);*/
	}

	// episode timeout
	if( maxEpisodeLength > 0 && episodeFrames > maxEpisodeLength )
	{
		printf("ArmPlugin - triggering EOE, episode has exceeded %i frames\n", maxEpisodeLength);

		rewardHistory = REWARD_LOSS;
		newReward     = true;
		endEpisode    = true;
	}

	// if an EOE reward hasn't already been issued, compute one
	if( hadNewState && !newReward )
	{
		PropPlugin* prop = GetPropByName(PROP_NAME);

		if( !prop )
		{
			printf("ArmPlugin - failed to find Prop '%s'\n", PROP_NAME);
			return;
		}

		// get the bounding box for the prop object
		const math::Box& propBBox = prop->model->GetBoundingBox();
		physics::LinkPtr gripper  = model->GetLink(GRIP_NAME);

		if( !gripper )
		{
			printf("ArmPlugin - failed to find Gripper '%s'\n", GRIP_NAME);
			return;
		}

		// if the robot impacts the ground, count it as a loss
		const math::Box& gripBBox = gripper->GetBoundingBox();
		const math::Box& gripLBBox = model->GetLink("gripperleft")->GetBoundingBox();
		const float groundContact = 0.1f;
		const float propGripRDist = BoxDistance(gripBBox, propBBox);
		const float propGripLDist = BoxDistance(gripLBBox, propBBox);
		printf("YOU ARE LOOKING FOR ME: %f, %f\n", propGripRDist, propGripLDist);
		

		if( gripBBox.min.z <= groundContact || gripBBox.max.z <= groundContact )
		{
			for( uint32_t n=0; n < 10; n++ )
				printf("GROUND CONTACT, EOE\n");

			rewardHistory = REWARD_LOSS;
			newReward     = true;
			endEpisode    = true;
		}
		else if( isPropInside(gripLBBox, gripBBox, propBBox))
		{
			printf("PROP INSIDE. ATTEMPTING TO GRASP\n");
			j2_controller->SetJointPosition(this->model->GetJoint("gripperright"), -0.025);
			j2_controller->SetJointPosition(this->model->GetJoint("gripperleft"),  0.025);

		}
		else
		{
			const float distGoal = BoxDistance(gripBBox, propBBox); // compute the reward from distance to the goal

			printf("distance('%s', '%s') = %f\n", gripper->GetName().c_str(), prop->model->GetName().c_str(), distGoal);

			if( episodeFrames > 1 )
			{
				const float distDelta  = lastGoalDistance - distGoal;
				const float distThresh = 1.5f;		// maximum distance to the goal
				const float epsilon    = 0.001f;		// minimum pos/neg change in position
				const float movingAvg  = 0.9f;

				avgGoalDelta = (avgGoalDelta * movingAvg) + (distDelta * (1.0f - movingAvg));
		
				printf("AVG GOAL DELTA  %f\n", avgGoalDelta);
		
				rewardHistory = avgGoalDelta;
#if 0
				if( avgGoalDelta > 0.001f )
				{
					/*float bboxFactor = distThresh - distGoal;

					if( bboxFactor < 0.0f )
						bboxFactor = 0.0f;

					bboxFactor = bboxFactor / distThresh; *///(bboxFactor * bboxFactor) / (distThresh * distThresh);

					rewardHistory = avgGoalDelta; //(avgGoalDelta * 25.0f) /** (bboxFactor * 1.0f + 0.1f)*/;
				}
				else
					rewardHistory = 0.0f;
#endif
		
	#if 0
				if( distDelta <= -epsilon )
					rewardHistory = -1.0f;
				else if( distDelta >= epsilon )
				{
					float bboxFactor = distThresh - distBBox;

					if( bboxFactor < 0.0f )
						bboxFactor = 0.0f;

					bboxFactor = (bboxFactor * bboxFactor) / (distThresh * distThresh);

					const float multiplier = (bboxFactor * 2.0f) + 0.1f;
			
					//bboxFactor /= distThresh;	// percentage of the way to the goal

					//const float multiplier = (((distThresh - distBBox) / distThresh) * 2.0f) + 1.0f;
			
					//if( multiplier < 0.25f )
					//	multiplier = 0.25f;

					rewardHistory = multiplier;

					//printf("MULTIPLIER %f\n", multiplier);
			
					//rewardHistory = 0.1f;
				} 
				else
					rewardHistory = 0.0f;
	#endif
				newReward = true;	
			}

			lastGoalDistance = distGoal;
		}	
	}

	// issue rewards and train DQN
	if( newReward && agent != NULL )
	{
		printf("ArmPlugin - issuing reward %f, EOE=%s  %s\n", rewardHistory, endEpisode ? "true" : "false", (rewardHistory > 0.1f) ? "POS+" : (rewardHistory > 0.0f) ? "POS" : (rewardHistory < 0.0f) ? "    NEG" : "       ZERO");
		agent->NextReward(rewardHistory, endEpisode);

		// reset reward indicator
		newReward = false;

		// reset for next episode
		if( endEpisode )
		{
			testAnimation    = true;	// reset the robot to base position
			loopAnimation    = false;
			endEpisode       = false;
			episodeFrames    = 0;
			lastGoalDistance = 0.0f;
			avgGoalDelta     = 0.0f;
			// ResetPropDynamics();  // now handled mid-reset sequence

			for( uint32_t n=0; n < DOF; n++ )
				vel[n] = 0.0f;
		}
	}
}


// Inverse kinematics solver
void IK( float x, float y, float theta[3] )
{
	const float l1 = 4000.0f;
	const float l2 = 4000.0f;
	const float l3 = 1000.0f;

	const float phi = 0.0f;

	const float xw = x - l3 * cosf(0.0f);
	const float yw = y - l3 * sinf(0.0f);

	const float l12 = l1 * l1;
	const float l22 = l2 * l2;

	const float xw2 = xw * xw;
	const float yw2 = yw * yw;

	if( xw == 0.0f && yw == 0.0f )
	{
		theta[0] = 0.0f;
		theta[1] = 0.0f;
		theta[2] = 0.0f;
	}
	else
	{
		theta[1] = PI - acosf((l12+l22-xw2-yw2)/(2*l1*l2));
		theta[0] = atanf(yw/xw) - acosf((l12-l22+xw2+yw2)/(2*l1*sqrtf(xw2+yw2)));
		theta[2] = phi - theta[1] - theta[0];
	}
}


// setAnimationTarget
void ArmPlugin::setAnimationTarget( float x, float y )
{
	IK( x, y, dT );

	printf("theta:  %f  %f  %f\n", dT[0], dT[1], dT[2]);
	
	dT[0] /= float(ANIMATION_STEPS);
	dT[1] /= float(ANIMATION_STEPS);
	dT[2] /= float(ANIMATION_STEPS);

	printf("dT:  %f  %f  %f\n", dT[0], dT[1], dT[2]);
}

}

