/**
 * @file   uwtwr.h
 * @author Yuehan Jiang
 * @version 1.0.0
 *
 * @brief Common structures and variables in the protocol
 */

#ifndef UWTWR_CMN_HDR_H
#define UWTWR_CMN_HDR_H

#include <mmac.h>
#include <module.h>
#include <packet.h>

#include <list>

#define HDR_POLL(p) \
	(hdr_POLL::access(p)) /**< alias defined to access the POLL HEADER */
#define HDR_ACK_NODE(p) \
	(hdr_ACK_NODE::access(p)) /**< alias defined to access the ACK SINK HEADER*/

extern packet_t PT_POLL;
extern packet_t PT_ACK_NODE;

// To be implemented
/** Single location of the POLL vector. Each POLL_ID represent a polled node */
typedef struct POLL_ID {
	int id_; /**< ID of the node */
	double t_wait_; /**< Time that a node has to wait before being polled */
} id_poll;

/**
 * Header of the POLL message
 */
typedef struct hdr_POLL {
	int id_; /**< ID of the POLLED node */
	uint POLL_uid_; /**< POLL packet unique ID */
	uint16_t POLL_time_; /**< Time needed by the AUV to poll all the nodes */
	static int offset_; /**< Required by the PacketHeaderManager. */

	// Info to be sent in a POLL
	double tof_; /**< TOF from last received ACK >*/

	// Hybrid protocol payload (sender_role_ 0: legacy POLL, the fields below are unused).
	// The air size of the POLL is POLL_size_, not the size of this struct.
	int sender_role_; /**< 0: legacy, 1: B1, 2: B2 */
	double buoy_x_; /**< Position of the sending buoy [m] */
	double buoy_y_;
	double buoy_z_;
	int r_node_; /**< node_id_ of the relayed range, -1 if none */
	double r_; /**< Relayed range [m]: r1(i) in B2's POLL, r2(i-1) in B1's POLL */
	double r_time_; /**< Measurement time of the relayed range [s] */
	int xhat_node_; /**< node_id_ of the position estimate, -1 if none (step 6) */
	double xhat_x_; /**< Position estimate [m] */
	double xhat_y_;
	double xhat_z_;
	double xhat_time_; /**< Time of the position estimate [s] */

	
	/**
	 * Reference to the id_ variable
	 */
	int &
	ID()
	{
		return (id_);
	}
	/**
	 * Reference to the POLL_uid_ variable
	 */
	uint &
	POLL_uid()
	{
		return (POLL_uid_);
	}

	/**
	 * Reference to the POLL_time variable
	 */
	uint16_t &
	POLL_time()
	{
		return (POLL_time_);
	}

	/**
	 * Reference to the offset variable
	 */
	inline static int &
	offset()
	{
		return offset_;
	}

	inline static struct hdr_POLL *
	access(const Packet *p)
	{
		return (struct hdr_POLL *) p->access(offset_);
	}

} hdr_POLL;

/**
 * Header of the ACK sent by the NODE
 */
typedef struct hdr_ACK_NODE {
	// std::vector<uint16_t> id_ack_; /**< ACK is the id of the wrong packets */
	uint id_node_; /**< ID of the node */
	uint ACK_uid_; /**< Unique ID of the ACK packet */
	static int offset_; /**< Required by the PacketHeaderManager. */

	// /**
	//  * Reference to the id_ack_ variable
	//  */
	// std::vector<uint16_t> &
	// id_ack()
	// {
	// 	return (id_ack_);
	// }

	/**
	 * Reference to id_node variable
	 */
	uint &
	id_node()
	{
		return (id_node_);
	}

	/**
	 * Reference to ACK_uid_ variable
	 */
	uint &
	ACK_uid()
	{
		return (ACK_uid_);
	}

	/**
	 * Reference to the offset variable
	 */
	inline static int &
	offset()
	{
		return offset_;
	}

	inline static struct hdr_ACK_NODE *
	access(const Packet *p)
	{
		return (struct hdr_ACK_NODE *) p->access(offset_);
	}
} hdr_ACK_NODE;

#endif