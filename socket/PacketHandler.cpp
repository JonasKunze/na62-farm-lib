/*
 * PacketHandler.cpp
 *
 *  Created on: Feb 7, 2012
 *      Author: Jonas Kunze (kunzej@cern.ch)
 */

#include "PacketHandler.h"

#include <asm-generic/errno-base.h>
#include <boost/date_time/posix_time/posix_time_duration.hpp>
#include <boost/date_time/time_duration.hpp>
#include <boost/thread/pthread/thread_data.hpp>
#include <glog/logging.h>
#include <linux/pf_ring.h>
#include <net/ethernet.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/types.h>
#include <zmq.h>
#include <zmq.hpp>
#include <algorithm>
#include <cstdbool>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <queue>

#include "../exceptions/UnknownCREAMSourceIDFound.h"
#include "../exceptions/UnknownSourceIDFound.h"
#include "../l0/MEP.h"
#include "../l0/MEPEvent.h"
#include "../LKr/L1DistributionHandler.h"
#include "../LKr/LKREvent.h"
#include "../LKr/LKRMEP.h"
#include "../options/Options.h"
#include "../structs/Event.h"
#include "../structs/Network.h"
#include "EthernetUtils.h"
#include "PFringHandler.h"
#include "ZMQHandler.h"

namespace na62 {

bool changeBurstID = false;
uint32_t nextBurstID = 0;

uint NUMBER_OF_EBS = 0;

std::atomic<uint64_t>* PacketHandler::MEPsReceivedBySourceID_;
std::atomic<uint64_t>* PacketHandler::EventsReceivedBySourceID_;
std::atomic<uint64_t>* PacketHandler::BytesReceivedBySourceID_;

PacketHandler::PacketHandler() {
	NUMBER_OF_EBS = Options::GetInt(OPTION_NUMBER_OF_EBS);
}

PacketHandler::~PacketHandler() {
	std::cout << "Deleting PacketHandler " << threadNum_ << std::endl;
	for (auto socket : EBL0sockets_) {
		socket->close();
		delete socket;
	}

	for (auto socket : EBLKrSockets_) {
		socket->close();
		delete socket;
	}
}

void PacketHandler::Initialize() {
	int highestSourceID = SourceIDManager::LARGEST_L0_DATA_SOURCE_ID;
	if (highestSourceID < SOURCE_ID_LKr) { // Add LKr
		highestSourceID = SOURCE_ID_LKr;
	}

	MEPsReceivedBySourceID_ = new std::atomic<uint64_t>[highestSourceID + 1];
	EventsReceivedBySourceID_ = new std::atomic<uint64_t>[highestSourceID + 1];
	BytesReceivedBySourceID_ = new std::atomic<uint64_t>[highestSourceID + 1];

	for (int i = 0; i <= highestSourceID; i++) {
		MEPsReceivedBySourceID_[i] = 0;
		EventsReceivedBySourceID_[i] = 0;
		BytesReceivedBySourceID_[i] = 0;
	}
}

void PacketHandler::connectZMQ() {
	for (uint i = 0; i < NUMBER_OF_EBS; i++) {
		EBL0sockets_.push_back(ZMQHandler::GenerateSocket(ZMQ_PUSH));
		ZMQHandler::ConnectInproc(EBL0sockets_[i],
				ZMQHandler::GetEBL0Address(i));
	}

	for (uint i = 0; i < NUMBER_OF_EBS; i++) {
		EBLKrSockets_.push_back(ZMQHandler::GenerateSocket(ZMQ_PUSH));
		ZMQHandler::ConnectInproc(EBLKrSockets_[i],
				ZMQHandler::GetEBLKrAddress(i));
	}
}

void PacketHandler::thread() {
	connectZMQ();

	DataContainer container;

	std::queue<DataContainer> dataContainers;
	register char* data; // = new char[MTU];
	struct pfring_pkthdr hdr;
	memset(&hdr, 0, sizeof(hdr));
	register int result = 0;
	int sleepMicros = 1;
	while (true) {
		result = 0;
		data = NULL;
		/*
		 * The actual  polling!
		 * Do not wait for incoming packets as this will block the ring and make sending impossible
		 */
		result = PFringHandler::GetNextFrame(&hdr, &data, 0, false, threadNum_);
		if (result == 1) {
			char* buff = new char[hdr.len];
			memcpy(buff, data, hdr.len);
			container = {buff, (uint16_t) hdr.len};
			dataContainers.push(std::move(container));
			sleepMicros = 1;
		} else if (dataContainers.empty()) {
			/*
			 * Use the time to send some packets
			 */
			if (cream::L1DistributionHandler::DoSendMRP(threadNum_)) {
				sleepMicros = 1;
				continue;
			}
			boost::this_thread::sleep(boost::posix_time::microsec(sleepMicros));
			if (sleepMicros < 10000) {
				sleepMicros *= 2;
			}
		} else {
			if (!processPacket(dataContainers.front())) {
				dataContainers.pop();
				return; // stop running
			}
			dataContainers.pop();
		}
	}
}

bool PacketHandler::checkFrame(struct UDP_HDR* hdr, uint16_t length) {
	/*
	 * Check IP-Header
	 */
	//				if (!EthernetUtils::CheckData((char*) &hdr->ip, sizeof(iphdr))) {
	//					LOG(ERROR) << "Packet with broken IP-checksum received");
	//					delete[] container.data;
	//					continue;
	//				}
	if (ntohs(hdr->ip.tot_len) + sizeof(ether_header) != length) {
		/*
		 * Does not need to be equal because of ethernet padding
		 */
		if (ntohs(hdr->ip.tot_len) + sizeof(ether_header) > length) {
			LOG(ERROR)<<
			"Received IP-Packet with less bytes than ip.tot_len field!";
			return false;
		}
	}

	/*
	 * Does not need to be equal because of ethernet padding
	 */
	if (ntohs(hdr->udp.len) + sizeof(ether_header) + sizeof(iphdr)
			> length) {
		LOG(ERROR)<<"Received UDP-Packet with less bytes than udp.len field!";
		return false;
	}

	//				/*
	//				 * Check UDP checksum
	//				 */
	//				if (!EthernetUtils::CheckUDP(hdr, (const char *) (&hdr->udp) + sizeof(struct udphdr), ntohs(hdr->udp.len) - sizeof(struct udphdr))) {
	//					LOG(ERROR) << "Packet with broken UDP-checksum received" );
	//					delete[] container.data;
	//					continue;
	//				}
	return true;
}

void PacketHandler::processARPRequest(struct ARP_HDR* arp) {
	/*
	 * Look for ARP requests asking for my IP
	 */
	if (arp->targetIPAddr == PFringHandler::GetMyIP()) { // This is asking for me
		struct DataContainer responseArp = EthernetUtils::GenerateARPv4(
				PFringHandler::GetMyMac(), arp->sourceHardwAddr,
				PFringHandler::GetMyIP(), arp->sourceIPAddr,
				ARPOP_REPLY);
		PFringHandler::SendFrameConcurrently(threadNum_, responseArp.data,
				responseArp.length);
		delete[] responseArp.data;
	}
}

bool PacketHandler::processPacket(DataContainer container) {
	uint16_t L0_Port = Options::GetInt(OPTION_L0_RECEIVER_PORT);
	uint16_t CREAM_Port = Options::GetInt(OPTION_CREAM_RECEIVER_PORT);

	try {
		struct UDP_HDR* hdr = (struct UDP_HDR*) container.data;
		uint16_t etherType = ntohs(hdr->eth.ether_type);
		uint8_t ipProto = hdr->ip.protocol;
		uint16_t destPort = ntohs(hdr->udp.dest);

		/*
		 * Check if we received an ARP request
		 */
		if (etherType != ETHERTYPE_IP || ipProto != IPPROTO_UDP) {
			if (etherType == ETHERTYPE_ARP) {
				processARPRequest((struct ARP_HDR*) container.data);
			}

			// Anyway delete the buffer afterwards
			delete[] container.data;
			return true;
		}

		/*
		 * Check checksum errors
		 */
		if (!checkFrame(hdr, container.length)) {
			delete[] container.data;
			return true;
		}

		const char * UDPPayload = container.data + sizeof(struct UDP_HDR);
		const uint16_t & dataLength = ntohs(hdr->udp.len)
				- sizeof(struct udphdr);

		/*
		 *  Now let's see what's insight the packet
		 */
		if (destPort == L0_Port) {

			/*
			 * L0 Data
			 * * Length is hdr->ip.tot_len-sizeof(struct udphdr) and not container.length because of ethernet padding bytes!
			 */
			l0::MEP* mep = new l0::MEP(UDPPayload, dataLength, container.data);

			MEPsReceivedBySourceID_[mep->getSourceID()]++;
			EventsReceivedBySourceID_[mep->getSourceID()] +=
					mep->getNumberOfEvents();
			BytesReceivedBySourceID_[mep->getSourceID()] += container.length;

			for (int i = mep->getNumberOfEvents() - 1; i >= 0; i--) {
				l0::MEPEvent* event = mep->getEvent(i);

				zmq::message_t zmqMessage((void*) event,
						event->getEventLength(), (zmq::free_fn*) nullptr);

				while (true) {
					try {
						EBL0sockets_[event->getEventNumber() % NUMBER_OF_EBS]->send(
								zmqMessage);
						break;
					} catch (const zmq::error_t& ex) {
						if (ex.num() != EINTR) { // try again if EINTR (signal caught)
							LOG(ERROR)<< ex.what();
							return false;
						}
					}
				}
			}

		} else if (destPort == CREAM_Port) {
			/*
			 * CREAM Data
			 * Length is hdr->ip.tot_len-sizeof(struct iphdr) and not container.length because of ethernet padding bytes!
			 */
			cream::LKRMEP* mep = new cream::LKRMEP(UDPPayload, dataLength,
					container.data);

			MEPsReceivedBySourceID_[SOURCE_ID_LKr]++;
			EventsReceivedBySourceID_[SOURCE_ID_LKr] +=
			mep->getNumberOfEvents();
			BytesReceivedBySourceID_[SOURCE_ID_LKr] += container.length;
			for (int i = mep->getNumberOfEvents() - 1; i >= 0; i--) {
				cream::LKREvent* event = mep->getEvent(i);
				zmq::message_t zmqMessage((void*) event,
						event->getEventLength(), (zmq::free_fn*) nullptr);

				while (true) {
					try {
						EBLKrSockets_[event->getEventNumber() % NUMBER_OF_EBS]->send(
								zmqMessage);
						break;
					} catch (const zmq::error_t& ex) {
						if (ex.num() != EINTR) { // try again if EINTR (signal caught)
							std::cerr << ex.what() << std::endl;
							return false;
						}
					}
				}

			}
		} else if (destPort == Options::GetInt(OPTION_EOB_BROADCAST_PORT)) {
			if (dataLength != sizeof(struct EOB_FULL_FRAME) - sizeof(UDP_HDR)) {
				LOG(ERROR)<<
				"Unrecognizable packet received at EOB farm broadcast Port!";
				delete[] container.data;
				return true;
			}
			EOB_FULL_FRAME* pack = (struct EOB_FULL_FRAME*) container.data;
			LOG(INFO) <<
			"Received EOB Farm-Broadcast. Will increment BurstID now to" << pack->finishedBurstID + 1;
			changeBurstID = true;
			nextBurstID = pack->finishedBurstID + 1;
		} else {
			/*
			 * Packet with unknown UDP port received
			 */
			LOG(ERROR) <<"Packet with unknown UDP port received: " << destPort;
			delete[] container.data;
			return true;
		}
	} catch (UnknownSourceIDFound const& e) {
		delete[] container.data;
	} catch (UnknownCREAMSourceIDFound const&e) {
		delete[] container.data;
	} catch (NA62Error const& e) {
		delete[] container.data;
	}
	return true;
}

//void PacketHandler::StartDemultiplexerThread() throw () {
//	l0::MEP* mep = NULL;
//	cream::LKRMEP* lkrMep = NULL;
//	unsigned int sleepMicros = 10;
//
//	while (true) {
//		mep = NULL;
//		lkrMep = NULL;
//
//		/*
//		 * First move L0 MEPEvents to release memory at the CREAM side
//		 * Do this as long as you can find a MEP within the input queues
//		 */
//
//		for (int thread = NumberOfHandlers - 1; thread >= 0; thread--) {
//			while (createdMEPs[thread].pop(mep)) {
//				/*
//				 * Found a non-empty queue -> send the MEPEvents to the EBs by popping until the queue is empty
//				 */
//				MEPsReceivedBySourceID_[mep->getSourceID()]++;
//				BytesReceivedBySourceID_[mep->getSourceID()] +=
//						mep->getLength();
//				for (int i = mep->getNumberOfEvents() - 1; i >= 0; i--) {
//					l0::MEPEvent* event = mep->getEvent(i);
//
//					EventsReceivedBySourceID_[mep->getSourceID()]++;
//					eventBuilders_[event->getEventNumber()
//							% Options::Instance()->NUMBER_OF_EBS]->pushMEPEvent(
//							event);
//				}
//			}
//		}
//
//		/*
//		 * If no new L0 MEP has arrived we can increment the BurstID if the broadcast has been received
//		 */
//		if (changeBurstID && mep == NULL) {
//			changeBurstID = false;
//			EventBuilder::SetNextBurstID (nextBurstID);
//		}
//		/*
//		 * Now move LKREvents
//		 * Do this only once so that we can check for L0MEPs again
//		 */
//		for (int thread = NumberOfHandlers - 1; thread >= 0; thread--) {
//			while (createdLKRMEPs[thread].pop(lkrMep)) {
//				/*
//				 * Found a non-empty queue -> send the LKREvents to the EBs
//				 */
//				MEPsReceivedBySourceID_[LKR_SOURCE_ID]++;
//				BytesReceivedBySourceID_[LKR_SOURCE_ID] += lkrMep->getLength();
//				for (int i = lkrMep->getNumberOfEvents() - 1; i >= 0; i--) {
//					cream::LKREvent* event = lkrMep->getEvent(i);
//					EventsReceivedBySourceID_[LKR_SOURCE_ID]++;
//					eventBuilders_[event->getEventNumber()
//							% Options::Instance()->NUMBER_OF_EBS]->pushLKREvent(
//							event);
//				}
//			}
//		}
//
//		if (mep == NULL && lkrMep == NULL) {
//			/*
//			 * No empty queue found -> fight the greenhouse effect and save energy!
//			 */
//			if (Options::DO_SHUTDOWN) {
//				std::cout << "Stopping StorageHandler Demultiplexer"
//						<< std::endl;
//				return;
//			}
//			boost::this_thread::sleep(boost::posix_time::microsec(sleepMicros));
//			if (sleepMicros < 10000) {
//				sleepMicros *= 2;
//			}
//		} else {
//			sleepMicros = 1000;
//		}
//	}
//}

}
/* namespace na62 */
