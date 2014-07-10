// Copyright  (C)  2009  Ruben Smits <ruben dot_cart smits at mech dot kuleuven dot be>,
// Copyright  (C)  2009  Wilm Decre <wilm dot decre at mech dot kuleuven dot be>

// Author: Ruben Smits, Wilm Decre
// Maintainer: Ruben Smits, Wilm Decre

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

#include <rtt/Component.hpp>

#include <rtt/Logger.hpp>
#include <kdl/frames.hpp>

#include "FRIComponent.hpp"
#include <tf_conversions/tf_kdl.h>

#include <netinet/in.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <rtt/os/TimeService.hpp>

#include <posix/pthread.h>

namespace lwr_fri {

using namespace RTT;

    FRIComponent::FRIComponent(const std::string& name) :
        TaskContext(name, PreOperational),
        m_socket(0),m_jac(LBR_MNJ), m_lost_sample_count(0), prop_max_lost_samples(5)
    {

	this->addPort("fromKRL", port_from_krl);
	this->addPort("toKRL", port_to_krl);

	this->addPort("events", port_events).doc(
			"Port through which discrete events are emitted");
	this->addPort("RobotState", port_robot_state).doc(
			"Port containing the status of the robot");
	this->addPort("FRIState", port_fri_state).doc(
			"Port containing the status of the FRI communication");

	this->addPort("JointState", port_joint_state);
	this->addPort("FriJointState", port_fri_joint_state);

	this->addPort("CartesianPosition", port_cart_pos_msr);
	this->addPort("CartesianPositionFrame", port_cart_frame_msr);
	this->addPort("CartesianWrench", port_cart_wrench_msr);

	this->addPort("FriJointImpedance", port_fri_joint_impedance);
	this->addPort("JointPositionCommand", port_joint_pos_command);
	this->addPort("JointVelocityCommand", port_joint_vel_command);
	this->addPort("JointEffortCommand", port_joint_effort_command);

	this->addPort("CartesianPositionCommand", port_cart_pos_command);
	this->addPort("CartesianVelocityCommand", port_cart_vel_command);
	this->addPort("CartesianWrenchCommand", port_cart_wrench_command);
	this->addPort("CartesianImpedanceCommand", port_cart_impedance_command);

	this->addPort("Jacobian", jacobianPort);


	this->addProperty("udp_port", prop_local_port);
	this->addProperty("max_lost_samples", prop_max_lost_samples).doc("Number of times the joint positions get updated using the last desired velocities without receiving new data on port.");
	/*
	 this->addProperty("control_mode", prop_control_mode).doc("1=JntPos, 2=JntVel, 3=JntTrq, 4=CartPos, 5=CartForce, 6=CartTwist");
	 */
	memset(&m_msr_data, 0, sizeof(m_msr_data) );
	memset(&m_cmd_data, 0, sizeof(m_cmd_data) );

	m_mon_mode = "e_fri_mon_mode";
	m_cmd_mode = "e_fri_cmd_mode";
	m_unknown_mode = "e_fri_unkown_mode";
	m_previousPos.resize(LBR_MNJ);
	m_msrVelocities.resize(LBR_MNJ);
	m_currentPos.resize(LBR_MNJ);

}

FRIComponent::~FRIComponent() {
}

bool FRIComponent::configureHook() {
	//Check the sizes of all data:
	if (!FRI_CHECK_SIZES_OK) {
		log(Error) << "Padding on this platform is not OK :(" << endlog();
		return false;
	}
	//Check the byte order and float representation:
	{
		FRI_PREPARE_CHECK_BYTE_ORDER;
		if (!FRI_CHECK_BYTE_ORDER_OK) {
			log(Error)
					<< "Byte order and float representations are not OK on this platform :("
					<< endlog();
			return false;
		}
	}

	// presize the events port
	//TODO: check whether we should use rt_string for the event_port
	port_events.setDataSample("         10        20        30");
	fri_state_last = 0;

	//Resizing dynamic size objects
	m_joint_states.name.resize(LBR_MNJ);
	m_joint_states.position.resize(LBR_MNJ);
	m_joint_states.effort.resize(LBR_MNJ);
	m_joint_states.header.frame_id = "dummy_frame_id";
	m_fri_joint_state.header.frame_id = "dummy_frame_id";

	jacobianPort.setDataSample(m_jac);

	for (unsigned int i = 0; i < LBR_MNJ; i++) {
        std::ostringstream ss;
		ss << "arm_" << i+1 <<"_joint";
		m_joint_states.name[i] = ss.str();
	}
	port_joint_state.setDataSample(m_joint_states);

	if (fri_create_socket() != 0)
		return false;

	provides()->addAttribute("counter", counter);
	return true;
}

bool FRIComponent::startHook() {
	pthread_set_mode_np(PTHREAD_WARNSW, 0);
	counter = 0;
	m_init = true;
	return true;
}

void FRIComponent::updateHook() {
	pthread_set_mode_np(PTHREAD_WARNSW, 0);
	//Read:
	if (fri_recv() == 0) {

		//Check state and fire event if changed
		if (m_msr_data.intf.state == FRI_STATE_MON || !isPowerOn() ) {
			if (fri_state_last != m_msr_data.intf.state) {
				//cout << "Switching to monitor mode." <<endl;
				port_events.write(m_mon_mode);
			}
		} else if (m_msr_data.intf.state == FRI_STATE_CMD && isPowerOn() ) {
			if (fri_state_last != m_msr_data.intf.state) {
				port_events.write(m_cmd_mode);
				//cout << "Switching to command mode." <<endl;
			}
		} else {
			if (fri_state_last != m_msr_data.intf.state)
				port_events.write(m_unknown_mode);
		}
		fri_state_last = m_msr_data.intf.state;

		for ( int i = 0; i < FRI_CART_VEC; i++)
		    for ( int j = 0; j < LBR_MNJ; j++)
				m_jac(i,j) = m_msr_data.data.jacobian[i*LBR_MNJ+j];
		//Kuka uses Tx, Ty, Tz, Rz, Ry, Rx convention, so we need to swap Rz and Rx
		m_jac.data.row(3).swap(m_jac.data.row(5));
		jacobianPort.write(m_jac);

        for ( unsigned int i = 0; i < LBR_MNJ; i++)
            for ( unsigned int j = 0; j < LBR_MNJ; j++)
                mass_matrix(i, j) = m_msr_data.data.massMatrix[i*LBR_MNJ+j];
        massMatrixPort.write(mass_matrix);

		//Put robot and fri state on the ports(no parsing)
		port_robot_state.write(m_msr_data.robot);
		port_fri_state.write(m_msr_data.intf);

		//Put KRL data onto the ports(no parsing)
		port_from_krl.write(m_msr_data.krl);

		// Fill in fri_joint_state and joint_state
		for (unsigned int i = 0; i < LBR_MNJ; i++) {
			m_fri_joint_state.msrJntPos[i] = m_msr_data.data.msrJntPos[i];
			m_fri_joint_state.cmdJntPos[i] = m_msr_data.data.cmdJntPos[i];
			m_fri_joint_state.cmdJntPosFriOffset[i]
					= m_msr_data.data.cmdJntPosFriOffset[i];
			m_fri_joint_state.msrJntTrq[i] = m_msr_data.data.msrJntTrq[i];
			m_fri_joint_state.estExtJntTrq[i] = m_msr_data.data.estExtJntTrq[i];
			m_fri_joint_state.gravity[i] = m_msr_data.data.gravity[i];
		}
		m_joint_states.position.assign(m_msr_data.data.msrJntPos,
				m_msr_data.data.msrJntPos + LBR_MNJ);
		m_joint_states.effort.assign(m_msr_data.data.estExtJntTrq,
				m_msr_data.data.estExtJntTrq + LBR_MNJ);
		/********** isir add, msr jnt velocities *********/

		for (unsigned int i = 0; i < LBR_MNJ; i++) {
			m_currentPos[i]=m_msr_data.data.msrJntPos[i];
		}
		for (unsigned int i = 0; i < LBR_MNJ; i++) {
			m_msrVelocities[i]=(m_currentPos[i]-m_previousPos[i])/(double)m_msr_data.intf.desiredMsrSampleTime;
		}
		for (unsigned int i = 0; i < LBR_MNJ; i++) {
			m_joint_states.velocity[i]=m_msrVelocities[i];
		}
		//m_joint_states.velocity.assign( m_msrVelocities,
		//		m_msrVelocities + LBR_MNJ );
		/************************* ***********************/
		m_joint_states.header.stamp.fromNSec ( RTT::os::TimeService::Instance()->getNSecs() );
		//m_joint_states.header.stamp.fromSec( m_msr_data.intf.timestamp ); --> only accurate to 1/10th of a second !!!
		m_fri_joint_state.header.stamp=m_joint_states.header.stamp;
		port_fri_joint_state.write(m_fri_joint_state);
		port_joint_state.write(m_joint_states);

		geometry_msgs::Quaternion quat;
		KDL::Frame cartPos;
		cartPos.M=KDL::Rotation(m_msr_data.data.msrCartPos[0],
		m_msr_data.data.msrCartPos[1], m_msr_data.data.msrCartPos[2],
		m_msr_data.data.msrCartPos[4], m_msr_data.data.msrCartPos[5],
		m_msr_data.data.msrCartPos[6], m_msr_data.data.msrCartPos[8],
		m_msr_data.data.msrCartPos[9], m_msr_data.data.msrCartPos[10]);
		cartPos.p.x(m_msr_data.data.msrCartPos[3]);
		cartPos.p.y(m_msr_data.data.msrCartPos[7]);
		cartPos.p.z(m_msr_data.data.msrCartPos[11]);
		tf::PoseKDLToMsg(cartPos,m_cartPos);
		port_cart_frame_msr.write(cartPos);
		port_cart_pos_msr.write(m_cartPos);

//		cartPos.M = KDL::Rotation(m_msr_data.data.cmdCartPos[0],
//		m_msr_data.data.cmdCartPos[1], m_msr_data.data.cmdCartPos[2],
//		 m_msr_data.data.cmdCartPos[4], m_msr_data.data.cmdCartPos[5],
//		 m_msr_data.data.cmdCartPos[6], m_msr_data.data.cmdCartPos[8],
//		 m_msr_data.data.cmdCartPos[9], m_msr_data.data.cmdCartPos[10]);
//		 cartPos.p.x(m_msr_data.data.cmdCartPos[3]);
//		 cartPos.p.y(m_msr_data.data.cmdCartPos[7]);
//		 cartPos.p.z(m_msr_data.data.cmdCartPos[11]);
//		 tf::PoseKDLToMsg(cartPos,m_cartPos);
//		 m_cmdCartPosPort.write(m_cartPos);

//		 cartPos.M = KDL::Rotation(m_msr_data.data.cmdCartPosFriOffset[0],
//		 m_msr_data.data.cmdCartPosFriOffset[1],
//		 m_msr_data.data.cmdCartPosFriOffset[2],
//		 m_msr_data.data.cmdCartPosFriOffset[4],
//		 m_msr_data.data.cmdCartPosFriOffset[5],
//		 m_msr_data.data.cmdCartPosFriOffset[6],
//		 m_msr_data.data.cmdCartPosFriOffset[8],
//		 m_msr_data.data.cmdCartPosFriOffset[9],
//		 m_msr_data.data.cmdCartPosFriOffset[10]);
//		 cartPos.p.x(m_msr_data.data.cmdCartPosFriOffset[3]);
//		 cartPos.p.y(m_msr_data.data.cmdCartPosFriOffset[7]);
//		 cartPos.p.z(m_msr_data.data.cmdCartPosFriOffset[11]);
//		 tf::PoseKDLToMsg(cartPos,m_cartPos);
//		 m_cmdCartPosFriOffsetPort.write(m_cartPos);

		 m_cartWrench.force.x = m_msr_data.data.estExtTcpFT[0];
		 m_cartWrench.force.y = m_msr_data.data.estExtTcpFT[1];
		 m_cartWrench.force.z = m_msr_data.data.estExtTcpFT[2];
		 m_cartWrench.torque.x = m_msr_data.data.estExtTcpFT[5];
		 m_cartWrench.torque.y = m_msr_data.data.estExtTcpFT[4];
		 m_cartWrench.torque.z = m_msr_data.data.estExtTcpFT[3];
		 port_cart_wrench_msr.write(m_cartWrench);


		//Fill in datagram to send:
		m_cmd_data.head.datagramId = FRI_DATAGRAM_ID_CMD;
		m_cmd_data.head.packetSize = sizeof(tFriCmdData);
		m_cmd_data.head.sendSeqCount = ++counter;
		m_cmd_data.head.reflSeqCount = m_msr_data.head.sendSeqCount;


		if (!isPowerOn()) {
			// necessary to write cmd if not powered on. See kuka FRI user manual p6 and friremote.cpp:
			for (int i = 0; i < LBR_MNJ; i++)
			{
				m_cmd_data.cmd.jntPos[i]=m_msr_data.data.cmdJntPos[i]+m_msr_data.data.cmdJntPosFriOffset[i];
			}
		}
		if (m_msr_data.intf.state == FRI_STATE_MON || !isPowerOn()) {
			// joint position control capable modes:
			if (m_msr_data.robot.control == FRI_CTRL_POSITION
					|| m_msr_data.robot.control == FRI_CTRL_JNT_IMP) {
				m_cmd_data.cmd.cmdFlags = FRI_CMD_JNTPOS;
				for (unsigned int i = 0; i < LBR_MNJ; i++) {
					// see note above with !isPowerOn()
					// the user manual speaks of 'mimic msr.data.msrCmdJntPos' which is ambiguous.
					// on the other hand, the friremote.cpp will send this whenever (!isPowerOn() || state != FRI_STATE_CMD)
					// so we mimic the kuka reference code here...
					m_cmd_data.cmd.jntPos[i] = m_msr_data.data.cmdJntPos[i]+m_msr_data.data.cmdJntPosFriOffset[i];
				}
			}
			// Additional flags are set in joint impedance mode:
			if ( m_msr_data.robot.control == FRI_CTRL_JNT_IMP ) {
				m_cmd_data.cmd.cmdFlags |= FRI_CMD_JNTTRQ;
				m_cmd_data.cmd.cmdFlags |= FRI_CMD_JNTSTIFF | FRI_CMD_JNTDAMP;
				for (unsigned int i = 0; i < LBR_MNJ; i++) {
					m_cmd_data.cmd.addJntTrq[i] = 0.0;
					m_cmd_data.cmd.jntStiffness[i] = 250;
					m_cmd_data.cmd.jntDamping[i]   = 0.7;
				}
			}
			if (m_msr_data.robot.control == FRI_CTRL_CART_IMP) {
				m_cmd_data.cmd.cmdFlags = FRI_CMD_CARTPOS | FRI_CMD_TCPFT;
				m_cmd_data.cmd.cmdFlags |= FRI_CMD_CARTSTIFF | FRI_CMD_CARTDAMP;
				for (unsigned int i = 0; i < FRI_CART_FRM_DIM; i++)
					m_cmd_data.cmd.cartPos[i] = m_msr_data.data.msrCartPos[i];
				for(unsigned int i = 0 ; i < FRI_CART_VEC ; i++)
					m_cmd_data.cmd.addTcpFT[i]=0.0;
				for(unsigned int i=0; i < FRI_CART_VEC/2 ; i++)
				{
					//Linear part;
					m_cmd_data.cmd.cartStiffness[i]=100;
					m_cmd_data.cmd.cartDamping[i]=0.1;
					//rotational part;
					m_cmd_data.cmd.cartStiffness[i+FRI_CART_VEC/2]=10;
					m_cmd_data.cmd.cartDamping[i+FRI_CART_VEC/2]=0.1;
				}
			}
		}
		//Only send if state is in FRI_STATE_CMD and drives are powerd
		if ( (m_msr_data.intf.state == FRI_STATE_CMD) && isPowerOn() ) {
			//Valid ports in joint position and joint impedance mode
			if (m_msr_data.robot.control == FRI_CTRL_POSITION
					|| m_msr_data.robot.control == FRI_CTRL_JNT_IMP) {
				//Read desired positions
				if (port_joint_pos_command.read(m_joint_pos_command) == NewData) {
					if (m_joint_pos_command.positions.size() == LBR_MNJ) {
						for (unsigned int i = 0; i < LBR_MNJ; i++)
							m_cmd_data.cmd.jntPos[i]
									= m_joint_pos_command.positions[i];
					} else
						log(Warning) << "Size of "
								<< port_joint_pos_command.getName()
								<< " not equal to " << LBR_MNJ << endlog();

				}

				//Count lost samples
				if (port_joint_vel_command.read(m_joint_vel_command) == NewData)
				    m_lost_sample_count = 0;
				else
				    m_lost_sample_count ++;

				//Read desired velocities
				if ( (port_joint_vel_command.read(m_joint_vel_command) != NoData) && (m_lost_sample_count < prop_max_lost_samples) ) {
					if (m_joint_vel_command.velocities.size() == LBR_MNJ) {
						for (unsigned int i = 0; i < LBR_MNJ; i++)
							m_cmd_data.cmd.jntPos[i]
									+= m_joint_vel_command.velocities[i]
											* m_msr_data.intf.desiredCmdSampleTime;
					} else
						log(Warning) << "Size of "
								<< port_joint_vel_command.getName()
								<< " not equal to " << LBR_MNJ << endlog();
				}
			}
			//Valid ports only in joint impedance mode
			if (m_msr_data.robot.control == FRI_CTRL_JNT_IMP) {
				//Read desired additional joint torques
				if (port_joint_effort_command.read(m_joint_effort_command)
						== NewData) {
					//Check size
					if (m_joint_effort_command.efforts.size() == LBR_MNJ) {
						for (unsigned int i = 0; i < LBR_MNJ; i++)
							m_cmd_data.cmd.addJntTrq[i]
									= m_joint_effort_command.efforts[i];
					} else
						log(Warning) << "Size of "
								<< port_joint_effort_command.getName()
								<< " not equal to " << LBR_MNJ << endlog();

				}
				//Read desired joint impedance
				if (port_fri_joint_impedance.read(m_fri_joint_impedance)
						== NewData) {
					for (unsigned int i = 0; i < LBR_MNJ; i++) {
						m_cmd_data.cmd.jntStiffness[i]
								= m_fri_joint_impedance.stiffness[i];
						m_cmd_data.cmd.jntDamping[i]
								= m_fri_joint_impedance.damping[i];
					}
				}
			} else if (m_msr_data.robot.control == FRI_CTRL_CART_IMP) {
				if (NewData == port_cart_pos_command.read(m_cartPos)) {
					KDL::Rotation rot =
							KDL::Rotation::Quaternion(
									m_cartPos.orientation.x, m_cartPos.orientation.y,
									m_cartPos.orientation.z, m_cartPos.orientation.w);
					m_cmd_data.cmd.cartPos[0] = rot.data[0];
					m_cmd_data.cmd.cartPos[1] = rot.data[1];
					m_cmd_data.cmd.cartPos[2] = rot.data[2];
					m_cmd_data.cmd.cartPos[4] = rot.data[3];
					m_cmd_data.cmd.cartPos[5] = rot.data[4];
					m_cmd_data.cmd.cartPos[6] = rot.data[5];
					m_cmd_data.cmd.cartPos[8] = rot.data[6];
					m_cmd_data.cmd.cartPos[9] = rot.data[7];
					m_cmd_data.cmd.cartPos[10] = rot.data[8];

					m_cmd_data.cmd.cartPos[3] = m_cartPos.position.x;
					m_cmd_data.cmd.cartPos[7] = m_cartPos.position.y;
					m_cmd_data.cmd.cartPos[11] = m_cartPos.position.z;
				}

				if (NewData == port_cart_wrench_command.read(m_cartWrench)) {
					m_cmd_data.cmd.addTcpFT[0] = m_cartWrench.force.x;
					m_cmd_data.cmd.addTcpFT[1] = m_cartWrench.force.y;
					m_cmd_data.cmd.addTcpFT[2] = m_cartWrench.force.z;
					m_cmd_data.cmd.addTcpFT[3] = m_cartWrench.torque.z;
					m_cmd_data.cmd.addTcpFT[4] = m_cartWrench.torque.y;
					m_cmd_data.cmd.addTcpFT[5] = m_cartWrench.torque.x;
				}

				if (NewData == port_cart_vel_command.read(m_cartTwist)) {
					KDL::Twist t;
					tf::TwistMsgToKDL (m_cartTwist, t);
					KDL::Frame T_old;
					T_old.M = KDL::Rotation(m_cmd_data.cmd.cartPos[0],
							m_cmd_data.cmd.cartPos[1],
							m_cmd_data.cmd.cartPos[2],
							m_cmd_data.cmd.cartPos[4],
							m_cmd_data.cmd.cartPos[5],
							m_cmd_data.cmd.cartPos[6],
							m_cmd_data.cmd.cartPos[8],
							m_cmd_data.cmd.cartPos[9],
							m_cmd_data.cmd.cartPos[10]);
					T_old.p.x(m_cmd_data.cmd.cartPos[3]);
					T_old.p.y(m_cmd_data.cmd.cartPos[7]);
					T_old.p.z(m_cmd_data.cmd.cartPos[11]);

					KDL::Frame T_new = addDelta (T_old, t, m_msr_data.intf.desiredCmdSampleTime);

					m_cmd_data.cmd.cartPos[0] = T_new.M.data[0];
					m_cmd_data.cmd.cartPos[1] = T_new.M.data[1];
					m_cmd_data.cmd.cartPos[2] = T_new.M.data[2];
					m_cmd_data.cmd.cartPos[4] = T_new.M.data[3];
					m_cmd_data.cmd.cartPos[5] = T_new.M.data[4];
					m_cmd_data.cmd.cartPos[6] = T_new.M.data[5];
					m_cmd_data.cmd.cartPos[8] = T_new.M.data[6];
					m_cmd_data.cmd.cartPos[9] = T_new.M.data[7];
					m_cmd_data.cmd.cartPos[10] = T_new.M.data[8];
					m_cmd_data.cmd.cartPos[3] = T_new.p.x();
					m_cmd_data.cmd.cartPos[7] = T_new.p.y();
					m_cmd_data.cmd.cartPos[11] = T_new.p.z();
				}
				if (NewData == port_cart_impedance_command.read(m_cartImp)){
					m_cmd_data.cmd.cartStiffness[0]=m_cartImp.stiffness.linear.x;
					m_cmd_data.cmd.cartStiffness[1]=m_cartImp.stiffness.linear.y;
					m_cmd_data.cmd.cartStiffness[2]=m_cartImp.stiffness.linear.z;
					m_cmd_data.cmd.cartStiffness[5]=m_cartImp.stiffness.angular.x;
					m_cmd_data.cmd.cartStiffness[4]=m_cartImp.stiffness.angular.y;
					m_cmd_data.cmd.cartStiffness[3]=m_cartImp.stiffness.angular.z;
					m_cmd_data.cmd.cartDamping[0]=m_cartImp.damping.linear.x;
					m_cmd_data.cmd.cartDamping[1]=m_cartImp.damping.linear.y;
					m_cmd_data.cmd.cartDamping[2]=m_cartImp.damping.linear.z;
					m_cmd_data.cmd.cartDamping[5]=m_cartImp.damping.angular.x;
					m_cmd_data.cmd.cartDamping[4]=m_cartImp.damping.angular.y;
					m_cmd_data.cmd.cartDamping[3]=m_cartImp.damping.angular.z;
				}
			} else if (m_msr_data.robot.control == FRI_CTRL_OTHER) {
				this->error();
			}
		}//End command mode

		port_to_krl.read(m_cmd_data.krl);

		fri_send();
		  //this->error();
		if(m_init){
			rtos_enable_rt_warning();
			m_init = false;
		}
	}//End fri_recv succesfull

	this->trigger();
}

void FRIComponent::stopHook() {
}

void FRIComponent::cleanupHook() {
	close(m_socket);
}

int FRIComponent::fri_create_socket() {
	if (m_socket != 0)
		close(m_socket);
	m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, 0, 0);

	struct sockaddr_in local_addr;
	bzero((char *) &local_addr, sizeof(local_addr));
	local_addr.sin_family = AF_INET;
	local_addr.sin_addr.s_addr = INADDR_ANY;
	local_addr.sin_port = htons(prop_local_port);

	if (bind(m_socket, (sockaddr*) &local_addr, sizeof(sockaddr_in)) < 0) {
		log(Error) << "Binding of port failed with errno " << errno << endlog();
		return -1;
	}

	return 0;
}

int FRIComponent::fri_recv() {
	int n = recvfrom(m_socket, (void*) &m_msr_data, sizeof(m_msr_data), 0,
			(sockaddr*) &m_remote_addr, &m_sock_addr_len);
	if (sizeof(tFriMsrData) != n) {
		log(Error) << "bad packet length: " << n << ", expected: "
				<< sizeof(tFriMsrData) << endlog();
		return -1;
	}
	return 0;
}

int FRIComponent::fri_send() {
	if (0 > sendto(m_socket, (void*) &m_cmd_data, sizeof(m_cmd_data), 0,
			(sockaddr*) &m_remote_addr, m_sock_addr_len)) {
		if (!m_init)
			log(Error) << "Sending datagram failed." << endlog();
		return -1;
	}
	return 0;
}

}//namespace LWR

ORO_CREATE_COMPONENT(lwr_fri::FRIComponent)

