//
// Copyright (c) 2017 Regents of the SIGNET lab, University of Padova.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
// 3. Neither the name of the University of Padova (SIGNET lab) nor the
//    names of its contributors may be used to endorse or promote products
//    derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
// TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

/**
 * @file   uwtwr_AAUV.cpp
 * @author Yuehan Jiang
 * @version 1.0.0
 *
 * @brief Provides the implementation of UWTWR_AAUV class
 */

#include <sstream>
#include "mac.h"
#include "uwcbr-module.h"
#include "uwtwr_AAUV.h"
#include "uwtwr_cmn_hdr.h"

#include <algorithm>
#include <float.h>
#include <fstream>
#include <sstream>
#include "uwphy-clmsg.h"
#include <iomanip>

static class UWTWR_AAUV_Class : public TclClass
{
public:
	UWTWR_AAUV_Class()
		: TclClass("Module/UW/TWR/AAUV")
	{
	}
	TclObject *
	create(int, const char *const *)
	{
		return (new UWTWR_AAUV());
	}
} class_uwtwr_aauv;

bool UWTWR_AAUV::initialized = false;

std::map<UWTWR_AAUV::UWTWR_AAUV_STATUS, std::string>
		UWTWR_AAUV::status_info;

UWTWR_AAUV::UWTWR_AAUV()
	: ack_timer(this)
	, poll_size(2) // Default to 2 nodes (configutable in TCL)
	, polling_rotation_index(0) // Start from first node
	, curr_poll_packet(0)
	, curr_ack_packet(0)
	, T_ack_timer(1) // send POLL every 1 second, need to be set in Tcl
	, curr_state(UWTWR_AAUV_STATUS_IDLE)
	, prev_state(UWTWR_AAUV_STATUS_IDLE) // not used
	, TxEnabled(false)
	, n_poll_tx(0)
	, n_ack_rx(0)
	, POLL_uid(0)
	, RxAckEnabled(false)
	, ack_enabled(1)
	, n_dropped_ack_pkts(0)
	, is_running(false)
	, custom_sequence(false)
	, n_pollers(1) // Default to a single poller (legacy protocol)
	, poller_index(0)
	, sound_speed(1500.0)
	, hybrid(0) // Hybrid protocol off by default
	, position(NULL)
	, partner(NULL)
	, latest_node(-1)
{
	bind("T_ack_timer_", (double *) &T_ack_timer);
	bind("ack_enabled_", (int *) &ack_enabled);
	bind("POLL_size_", (int *) &POLL_size);
	bind("ACK_size_", (int *) &ACK_size);
	bind("poll_size_", (int *) &poll_size);
	bind("n_pollers_", (int *) &n_pollers);
	bind("poller_index_", (int *) &poller_index);
	bind("sound_speed_", (double *) &sound_speed);
	bind("hybrid_", (int *) &hybrid);
}

UWTWR_AAUV::~UWTWR_AAUV()
{
}

int UWTWR_AAUV::command(int argc, const char *const * argv)
{
	Tcl &tcl = Tcl::instance();

	if (argc == 2) {
		if (strcasecmp(argv[1], "initialize") == 0) {
			// Initialize poll_node_ids with default sequence based on poll_size,
			// unless setPollSequence already set a custom one
			if (!custom_sequence) {
				poll_node_ids.clear();
				for (int i = 0; i < poll_size; i++) {
					poll_node_ids.push_back(i); // Poll nodes 0, 1, ..., poll_size-1
				}
			}
			if (!initialized)
				initInfo();
			return TCL_OK;
		} else if (strcasecmp(argv[1], "run") == 0) {
			if (n_pollers < 1 || poller_index < 0 || poller_index >= n_pollers) {
				std::cerr << "UWTWR_AAUV(" << addr << ")::run: invalid schedule, "
						  << "poller_index_ = " << poller_index
						  << " must be in [0, n_pollers_ - 1], n_pollers_ = "
						  << n_pollers << std::endl;
				return TCL_ERROR;
			}
			if (hybrid && (n_pollers != 2 || partner == NULL || position == NULL ||
						!partner->hybrid || partner->poller_index == poller_index)) {
				std::cerr << "UWTWR_AAUV(" << addr << ")::run: invalid hybrid setup, "
						  << "needs n_pollers_ 2, setPosition, setPartner, and a partner "
						  << "with hybrid_ 1 and the other poller_index_" << std::endl;
				return TCL_ERROR;
			}
			if (poll_node_ids.empty() || poll_size != (int) poll_node_ids.size()) {
				std::cerr << "UWTWR_AAUV(" << addr << ")::run: empty or inconsistent "
						  << "poll sequence (" << poll_node_ids.size()
						  << " nodes, poll_size_ = " << poll_size
						  << "), call initialize or setPollSequence first" << std::endl;
				return TCL_ERROR;
			}
			is_running = true;
			if (poller_index > 0) {
				// wait for this poller's slot before the first POLL
				TxEnabled = false;
				ack_timer.schedule(poller_index * T_ack_timer);
			} else {
				stateIdle();
			}
			return TCL_OK;
		} else if (strcasecmp(argv[1], "stop") == 0) {
			is_running = false;
			if (ack_timer.isActive()) {
				ack_timer.stop();
			}
			RxAckEnabled = false;
			TxEnabled = false;
			return TCL_OK;
		} else if (strcasecmp(argv[1], "getAckRx") == 0) {
			tcl.resultf("%d", getAckRx());
			return TCL_OK;
		} else if (strcasecmp(argv[1], "getDroppedAckPkts") == 0) {
			tcl.resultf("%d", getDroppedAckPkts());
			return TCL_OK;
		} else if (strcasecmp(argv[1], "getPollSent") == 0) {
			tcl.resultf("%d", getPollSent());
			return TCL_OK;
		} else if (strcasecmp(argv[1], "getRelay") == 0) {
			// range this buoy would relay now: "node range time", or "-1 -1 -1"
			int node;
			RangeMeas m;
			if (getRelayedRange(node, m))
				tcl.resultf("%d %.9f %.9f", node, m.range, m.time);
			else
				tcl.resultf("-1 -1 -1");
			return TCL_OK;
		}
	} 
	if (argc >= 3) {
		if (strcasecmp(argv[1], "setMacAddr") == 0) {
			addr = atoi(argv[2]);
			return TCL_OK;
		}
		// set custom polling sequence
		if (strcasecmp(argv[1], "setPollSequence") == 0) {
			std::vector<int> ids;
			for (int i = 2; i < argc; i++) {
				char *end;
				long id = strtol(argv[i], &end, 10);
				if (end == argv[i] || *end != '\0' || id < 0) {
					std::cerr << "UWTWR_AAUV(" << addr << ")::setPollSequence: "
							  << argv[i] << " is not a node ID" << std::endl;
					return TCL_ERROR;
				}
				ids.push_back((int) id);
			}
			poll_node_ids = ids;
			poll_size = poll_node_ids.size();
			polling_rotation_index = 0;
			custom_sequence = true;
			return TCL_OK;
		}
	}
	if (argc == 3) {
		// position of this buoy, read at every POLL
		if (strcasecmp(argv[1], "setPosition") == 0) {
			position = dynamic_cast<Position *>(TclObject::lookup(argv[2]));
			if (position == NULL) {
				std::cerr << "UWTWR_AAUV(" << addr << ")::setPosition: "
						  << argv[2] << " is not a Position" << std::endl;
				return TCL_ERROR;
			}
			return TCL_OK;
		}
		// other buoy of the pair (ideal WiFi link), linked in both directions
		if (strcasecmp(argv[1], "setPartner") == 0) {
			UWTWR_AAUV *p = dynamic_cast<UWTWR_AAUV *>(TclObject::lookup(argv[2]));
			if (p == NULL || p == this) {
				std::cerr << "UWTWR_AAUV(" << addr << ")::setPartner: "
						  << argv[2] << " is not another Module/UW/TWR/AAUV" << std::endl;
				return TCL_ERROR;
			}
			partner = p;
			p->partner = this;
			return TCL_OK;
		}
		// last range measured to a node: "range time", or "-1 -1" if none
		if (strcasecmp(argv[1], "getLastRange") == 0) {
			std::map<int, RangeMeas>::const_iterator it =
					last_range.find(atoi(argv[2]));
			if (it == last_range.end())
				tcl.resultf("-1 -1");
			else
				tcl.resultf("%.9f %.9f", it->second.range, it->second.time);
			return TCL_OK;
		}
	}
	return MMac::command(argc, argv);
}

int UWTWR_AAUV::crLayCommand(ClMessage *m)
{
	switch (m->type()) {
		default:
			return MMac::crLayCommand(m);
	}
}

void UWTWR_AAUV::ACKTimer::expire(Event *e)
{
	timer_status = UWTWR_AAUV_EXPIRED;
	module->AckTOExpired();
}

void UWTWR_AAUV::AckTOExpired()
{
	RxAckEnabled = false;
	// go back to IDLE and poll the other node only if still running
	if (is_running) {
		stateIdle();
	}
}

// Is it not needed if no data packets to send
// void UWTWR_AAUV::recvFromUpperLayers(Packet *p)
// {
// }

void UWTWR_AAUV::Phy2MacStartRx(const Packet *p)
{
	hdr_cmn *cmh = hdr_cmn::access(p);
	hdr_MPhy *ph = hdr_MPhy::access(p);
	double gen_time = ph->txtime;
	double received_time = ph->rxtime;
	double tof = received_time - gen_time;
	// legacy (single poller): any received packet updates diff_time.
	// With several pollers, only a valid ACK does (see Phy2MacEndRx)
	if (n_pollers == 1)
		diff_time = tof;
	if (cmh->ptype() == PT_ACK_NODE) {
		if (debug_)
			std::cout << POLL_uid << "::" <<std::fixed<< std::setprecision(9) << NOW << "::UWTWR_AAUV(" << addr
					  << ")::RX_ACK_PACKET::TOF::" << tof << std::endl;
	}
}

void UWTWR_AAUV::Phy2MacEndRx(Packet *p)
{
	hdr_cmn *cmh = hdr_cmn::access(p);
	hdr_mac *mach = HDR_MAC(p);
	hdr_MPhy *ph = hdr_MPhy::access(p);
	int dest_mac = mach->macDA();
	double gen_time = ph->txtime;
	double received_time = ph->rxtime;
	double tof = received_time - gen_time;
	// legacy (single poller): any received packet updates diff_time
	if (n_pollers == 1)
		diff_time = tof;

	if (cmh->error()) {
		if (cmh->ptype_ == PT_ACK_NODE) {
			if (debug_)
				std::cout << POLL_uid << "::" <<std::fixed<< std::setprecision(9) << NOW << "::UWTWR_AAUV(" << addr
						<< ")::PHY2MACENDRX_DROP_ACK" << std::endl;
			incrDroppedAckPkts();
		} else {
			if (debug_)
				std::cout << POLL_uid << "::" <<std::fixed<< std::setprecision(9) << NOW << "::UWTWR_AAUV(" << addr
						<< ")::PHY2MACENDRX_DROP_POLL" << std::endl;
			incrErrorPktsRx();
		}
	} else {
		if ((dest_mac == addr) || (dest_mac == (int) MAC_BROADCAST)) {
			if (cmh->ptype() == PT_ACK_NODE && ack_enabled) {
				incrAckRx();
				// a range is valid only for the ACK of the node polled by this
				// poller, received while waiting for it
				hdr_ACK_NODE *ackh = HDR_ACK_NODE(p);
				if (dest_mac == addr && RxAckEnabled &&
						ackh->id_node_ == curr_node_id) {
					if (n_pollers > 1)
						diff_time = tof;
					storeRange(ackh->id_node_, tof * sound_speed, gen_time);
				}
				curr_ack_packet = p->copy();
				Packet::free(p);
				stateRxAck();
				//check ACK value and discard acked packets
			} 
		} else {
			if (debug_)
				std::cout << POLL_uid << "::" <<std::fixed<< std::setprecision(9) << NOW << "::UWTWR_AAUV(" << addr
						  << ")::PHY2MACENDRX::DROP_ACK_WRONG_DEST_"
						  << mach->macDA() << std::endl;
			incrXCtrlPktsRx();
			drop(p, 1, UWPOLLING_AUV_DROP_REASON_WRONG_RECEIVER);
		}
	}
}

void UWTWR_AAUV::Mac2PhyStartTx(Packet *p)
{
	hdr_cmn *cmh = hdr_cmn::access(p);
	if (cmh->ptype() == PT_POLL) {
		stateWaitAck();
	} else {
		stateWaitAck();
	}
	MMac::Mac2PhyStartTx(p);
}

void UWTWR_AAUV::Phy2MacEndTx(const Packet *p)
{
	// if (debug_)
	// 	std::cout << POLL_uid << "::" <<std::fixed<< std::setprecision(9) << NOW << "::UWTWR_AAUV(" << addr
	// 			  << ")::PHYEndTx" << std::endl;
}

void UWTWR_AAUV::stateTxPoll()
{
	if(TxEnabled) {
		SetNodePoll();
		refreshState(UWTWR_AAUV_STATUS_TX_POLL);
		Packet *p = Packet::alloc();
		hdr_cmn *cmh = hdr_cmn::access(p);
		hdr_mac *mach = HDR_MAC(p);
		// hdr_POLL *pollh = HDR_POLL(p);
		cmh->ptype() = PT_POLL;

		//Set the size to POLL_size with correct bytes number
		cmh->size() = POLL_size;
		mach->set(MF_CONTROL, addr, MAC_BROADCAST);
		mach->macSA() = addr;
		mach->macDA() = MAC_BROADCAST;
		curr_poll_packet = p->copy();
		Packet::free(p);
		hdr_POLL *pollh = HDR_POLL(curr_poll_packet);
		POLL_uid++;
		pollh->POLL_uid_ = POLL_uid;
		pollh->id_ = curr_node_id;

		// Save info (tof) to be sent in POLL in header (easier for now)
		pollh->tof_ = diff_time;

		// hybrid payload: legacy POLL by default (node IDs -1, since 0 is a valid node)
		pollh->sender_role_ = 0;
		pollh->r_node_ = -1;
		pollh->xhat_node_ = -1;
		if (hybrid) {
			pollh->sender_role_ = poller_index + 1;
			pollh->buoy_x_ = position->getX();
			pollh->buoy_y_ = position->getY();
			pollh->buoy_z_ = position->getZ();
			int r_node;
			RangeMeas r;
			if (getRelayedRange(r_node, r)) {
				pollh->r_node_ = r_node;
				pollh->r_ = r.range;
				pollh->r_time_ = r.time;
			}
		}

		if (debug_)
			std::cout << POLL_uid << "::" <<std::fixed<< std::setprecision(9) << NOW << "::UWTWR_AAUV(" << addr
						<< ")::STATE_TX_POLL::NODE::" << pollh->id_
						<< std::endl;
		// hybrid protocol: payload as written in the POLL header
		if (hybrid && debug_) {
			bool has_r = (pollh->r_node_ >= 0);
			std::cout << POLL_uid << "::" <<std::fixed<< std::setprecision(9) << NOW << "::UWTWR_AAUV(" << addr
					  << ")::TX_PAYLOAD::ROLE::" << pollh->sender_role_
					  << "::BUOY_POS::" << pollh->buoy_x_ << "::" << pollh->buoy_y_
					  << "::" << pollh->buoy_z_
					  << "::R_NODE::" << pollh->r_node_
					  << "::R::" << (has_r ? pollh->r_ : -1.0)
					  << "::R_TIME::" << (has_r ? pollh->r_time_ : -1.0)
					  << "::XHAT_NODE::" << pollh->xhat_node_ << std::endl;
		}
		TxPoll();
	} else {
		if (debug_)
			std::cerr << POLL_uid << "::" <<std::fixed<< std::setprecision(9) << NOW << "::UWTWR_AAUV(" << addr
					  << ")---> going in stateTxPoll from WRONG STATE---> "
						"current_state: "
					  << status_info[curr_state] << std::endl;
	}
}

void UWTWR_AAUV::TxPoll()
{
	// if(debug_)
	// 	std::cout << POLL_uid << "::" <<std::fixed<< std::setprecision(9) << NOW << "::UWTWR_AAUV(" << addr << ")::TX_POLL"
	// 			  << std::endl;
	incrCtrlPktsTx();
	incrPollTx();
	Mac2PhyStartTx(curr_poll_packet);
}

void UWTWR_AAUV::storeRange(int node, double range, double t_meas)
{
	RangeMeas &m = last_range[node];
	m.range = range;
	m.time = t_meas;
	// latest own range, read by the partner for the relay (hybrid protocol)
	latest_node = node;
	if (debug_)
		std::cout << POLL_uid << "::" <<std::fixed<< std::setprecision(9) << NOW << "::UWTWR_AAUV(" << addr
				  << ")::RANGE::NODE::" << node << "::R::" << range
				  << "::T_MEAS::" << t_meas << std::endl;
}

bool UWTWR_AAUV::getRelayedRange(int &node, RangeMeas &m) const
{
	node = -1;
	if (partner == NULL || partner->latest_node < 0)
		return false;
	std::map<int, RangeMeas>::const_iterator it =
			partner->last_range.find(partner->latest_node);
	if (it == partner->last_range.end())
		return false;
	// fresh only if from the partner's last exchange (within n_pollers slots)
	if (NOW - it->second.time > n_pollers * T_ack_timer)
		return false;
	node = partner->latest_node;
	m = it->second;
	return true;
}

void UWTWR_AAUV::SetNodePoll()
{
	// Get current node to poll from rotation list
	curr_node_id = poll_node_ids[polling_rotation_index];

	// Move to next node in rotation
	polling_rotation_index = (polling_rotation_index + 1) % poll_size;
}

// when to set the state to idle? TCl run--idle, waitAck--idle, RxAck--idle
void UWTWR_AAUV::stateIdle()
{
	refreshState(UWTWR_AAUV_STATUS_IDLE);
	// if (debug_)
	// 	std::cout << POLL_uid << "::" <<std::fixed<< std::setprecision(9) << NOW << "::UWTWR_AAUV(" << addr << ")::IDLE STATE "
	// 			  << std::endl;
	TxEnabled = true;
	// Only continue polling if is_running flag is true
	if (is_running) {
		stateTxPoll();
	}
}

void UWTWR_AAUV::initInfo()
{
	initialized = true;

	status_info[UWTWR_AAUV_STATUS_IDLE] = "Idle state";
	status_info[UWTWR_AAUV_STATUS_TX_POLL] = "Transmitting a POLL Packet";
	status_info[UWTWR_AAUV_STATUS_WAIT_ACK] = "Waiting for an ACK Packet";
	status_info[UWTWR_AAUV_STATUS_RX_ACK] = "Receiving ACK Packet";
}

void UWTWR_AAUV::stateWaitAck()
{
	refreshState(UWTWR_AAUV_STATUS_WAIT_ACK);
	TxEnabled = false;
	RxAckEnabled = true;
	// if (debug_)
	// 	std::cout << POLL_uid << "::" <<std::fixed<< std::setprecision(9) << NOW << "::UWTWR_AAUV(" << addr
	// 			  << ")::scheduling_ACK_TIMER_T= " << T_ack_timer << std::endl;
	// one POLL every n_pollers slots, the other slots belong to the other pollers
	ack_timer.schedule(n_pollers * T_ack_timer);
}

void UWTWR_AAUV::stateRxAck()
{
	if(RxAckEnabled) {
		// if (debug_)
		// 	std::cout << POLL_uid << "::" <<std::fixed<< std::setprecision(9) << NOW << "::UWTWR_AAUV(" << addr 
		// 			<< ")::stateRxAck()ACK received" << std::endl;
		refreshState(UWTWR_AAUV_STATUS_RX_ACK);
		RxAckEnabled = false;
		// handleAck();

		// maybe should let ack_timer expire
		// ack_timer.force_cancel();
		// stateIdle();
	}
	
}