/* -*-	Mode:C++; c-basic-offset:8; tab-width:8; indent-tabs-mode:t -*- 
 *
 * Copyright (c) 1996 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the Computer Systems
 *	Engineering Group at Lawrence Berkeley Laboratory and the Daedalus
 *	research group at UC Berkeley.
 * 4. Neither the name of the University nor of the Laboratory may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Header: /var/lib/cvs/ns-2.29/ucbt-0.9.8.2a/ns-2.29/wireless-phy.cc,v 1.5 2007/01/08 22:38:44 gauthier Exp $
 *
 * Ported from CMU/Monarch's code, nov'98 -Padma Haldar.
 * wireless-phy.cc
 * -----------------------------------------------------------------------
 * 03/14/06 - rouil/chevrollier - added interference functions
 */

#include <math.h>

#include <packet.h>

#include <mobilenode.h>
#include <phy.h>
#include <propagation.h>
#include <modulation.h>
#include <omni-antenna.h>
#include <wireless-phy.h>
#include <packet.h>
#include <ip.h>
#include <agent.h>
#include <trace.h>

#include "diffusion/diff_header.h"

//NIST
#include "interference.h"

#define MAX(a,b) (((a)<(b))?(b):(a))

#define MAXCOLLISIONWINDOW 0.01

void Sleep_Timer::expire(Event *) {
	a_->UpdateSleepEnergy();
}


/* ======================================================================
   WirelessPhy Interface
   ====================================================================== */

/* NIST */
static bool interferenceEnable_;
/* END NIST */

static class WirelessPhyClass: public TclClass {
public:
        WirelessPhyClass() : TclClass("Phy/WirelessPhy") {}
        TclObject* create(int, const char*const*) {
                return (new WirelessPhy);
        }
} class_WirelessPhy;


PacketQueue WirelessPhy::interfQ_[14];
int WirelessPhy::chan_ = 3;
int WirelessPhy::txqueued_ = 0;

WirelessPhy::WirelessPhy() : Phy(), sleep_timer_(this), status_(IDLE)
{
	/*
	 *  It sounds like 10db should be the capture threshold.
	 *
	 *  If a node is presently receiving a packet a a power level
	 *  Pa, and a packet at power level Pb arrives, the following
	 *  comparion must be made to determine whether or not capture
	 *  occurs:
	 *
	 *    10 * log(Pa) - 10 * log(Pb) > 10db
	 *
	 *  OR equivalently
	 *
	 *    Pa/Pb > 10.
	 *
	 */
	bind("CPThresh_", &CPThresh_);
	bind("CSThresh_", &CSThresh_);
	bind("RXThresh_", &RXThresh_);
	//bind("bandwidth_", &bandwidth_);
	bind("Pt_", &Pt_);
	bind("freq_", &freq_);
	bind("L_", &L_);
	
	lambda_ = SPEED_OF_LIGHT / freq_;

	node_ = 0;
	ant_ = 0;
	propagation_ = 0;
	modulation_ = 0;

	// Assume AT&T's Wavelan PCMCIA card -- Chalermek
        //	Pt_ = 8.5872e-4; // For 40m transmission range.
	//      Pt_ = 7.214e-3;  // For 100m transmission range.
	//      Pt_ = 0.2818; // For 250m transmission range.
	//	Pt_ = pow(10, 2.45) * 1e-3;         // 24.5 dbm, ~ 281.8mw
	
	Pt_consume_ = 0.660;  // 1.6 W drained power for transmission
	Pr_consume_ = 0.395;  // 1.2 W drained power for reception

	//	P_idle_ = 0.035; // 1.15 W drained power for idle

	P_idle_ = 0.0;
	P_sleep_ = 0.00;
	P_transition_ = 0.00;
	node_on_=1;
	
	channel_idle_time_ = NOW;
	update_energy_time_ = NOW;
	last_send_time_ = NOW;
	
	sleep_timer_.resched(1.0);
	//NIST: add 802.11 channel frequency.
	techno_ = UNKNOWN_PKT_TYPE;
	pktRx_ = NULL; //packet being successfully received
	expiration_ = 0; //end of packet
	//end NIST
}

int
WirelessPhy::command(int argc, const char*const* argv)
{
	TclObject *obj; 

	if (argc==2) {
		if (strcasecmp(argv[1], "NodeOn") == 0) {
			node_on();

			if (em() == NULL) 
				return TCL_OK;
			if (NOW > update_energy_time_) {
				update_energy_time_ = NOW;
			}
			return TCL_OK;
		} else if (strcasecmp(argv[1], "NodeOff") == 0) {
			node_off();

			if (em() == NULL) 
				return TCL_OK;
			if (NOW > update_energy_time_) {
				em()->DecrIdleEnergy(NOW-update_energy_time_,
						     P_idle_);
				update_energy_time_ = NOW;
			}
			return TCL_OK;
                } else if (strcasecmp(argv[1], "exportInterference") == 0) {
                        txqueued_ = 1;
                        return TCL_OK;
                } else if (strcasecmp(argv[1], "notExportInterference") == 0) {
                        txqueued_ = 0;
                        return TCL_OK;
 		} /*NIST: add get of channel number*/
                else if (strcmp (argv[1], "enableInterference") == 0) {
			setIntererenceEnabled();
			return TCL_OK;
		}
                /*end NIST*/
	} else if(argc == 3) {
		if (strcasecmp(argv[1], "setTxPower") == 0) {
			Pt_consume_ = atof(argv[2]);
			return TCL_OK;
		} else if (strcasecmp(argv[1], "setRxPower") == 0) {
			Pr_consume_ = atof(argv[2]);
			return TCL_OK;
		} else if (strcasecmp(argv[1], "setIdlePower") == 0) {
			P_idle_ = atof(argv[2]);
			return TCL_OK;
		}else if (strcasecmp(argv[1], "setSleepPower") == 0) {
			P_sleep_ = atof(argv[2]);
			return TCL_OK;
		} else if (strcasecmp(argv[1], "setTransitionPower") == 0) {
			P_transition_ = atof(argv[2]);
			return TCL_OK;
		} else if (strcasecmp(argv[1], "setTransitionTime") == 0) {
			T_transition_ = atof(argv[2]);
			return TCL_OK;
		/*NIST: add set-freq */
		} else if (strcmp(argv[1], "set-freq") == 0) {
                        freq_ = atof(argv[2]);
			lambda_ = SPEED_OF_LIGHT / freq_;
                        return TCL_OK;

		/*NIST: add set-channel */	
                /*
                } else if (strcmp(argv[1], "set-channel") == 0) {
                        int channelId = atoi(argv[2]);
                        //temporary hack for channel 1 and 6
                        if(channelId == 1){
                                RXThresh_ = 5.25089e-10;
                        }
                        if(channelId == 6){
                                RXThresh_ =  5.9978e-09;
                        }
                        CSThresh_ = RXThresh_ * 0.9;
                        channelId--;
                        freq_ = channel_freq_[channelId];
		*/
                        
		} else if (strcasecmp(argv[1], "setTechno") == 0) {
			if (strcasecmp(argv[2], "802.11") == 0) {
				techno_ = WLAN_PKT_TYPE;
			} else if (strcasecmp(argv[2], "802.15.4") == 0) {
				techno_ = WPAN_PKT_TYPE;
			} else if (strcasecmp(argv[2], "802.16") == 0) {
				techno_ = WIMAX_PKT_TYPE;
			} else if (strcasecmp(argv[2], "UMTS") == 0) {
				techno_ = UMTS_PKT_TYPE;
			} else if (strcasecmp(argv[2], "BT") == 0) {
				techno_ = BT_PKT_TYPE;
			} else {
				fprintf(stderr,"WirelessPhy: unknown techno %s\n", argv[2]);
				return TCL_ERROR;
			}
			return TCL_OK;
/*end NIST*/
		}else if( (obj = TclObject::lookup(argv[2])) == 0) {
			fprintf(stderr,"WirelessPhy: %s lookup of %s failed\n", 
				argv[1], argv[2]);
			return TCL_ERROR;
		}else if (strcmp(argv[1], "propagation") == 0) {
			assert(propagation_ == 0);
			propagation_ = (Propagation*) obj;
			return TCL_OK;
		} else if (strcasecmp(argv[1], "antenna") == 0) {
			ant_ = (Antenna*) obj;
			return TCL_OK;
		} else if (strcasecmp(argv[1], "node") == 0) {
			assert(node_ == 0);
			node_ = (Node *)obj;
			return TCL_OK;
		}
	}
	return Phy::command(argc,argv);
}

bool 
WirelessPhy::isInterferenceEnabled()
{
	return interferenceEnable_;
}

void 
WirelessPhy::setIntererenceEnabled()
{
	interferenceEnable_ = true;
}

void 
WirelessPhy::sendDown(Packet *p)
{
	/*
	 * Sanity Check
	 */
	assert(initialized());
	
	if (em()) {
			//node is off here...
			if (Is_node_on() != true ) {
			Packet::free(p);
			return;
			}
			if(Is_node_on() == true && Is_sleeping() == true){
			em()-> DecrSleepEnergy(NOW-update_energy_time_,
							P_sleep_);
			update_energy_time_ = NOW;

			}

	}
	/*
	 * Decrease node's energy
	 */
	if(em()) {
		if (em()->energy() > 0) {

		    double txtime = hdr_cmn::access(p)->txtime();
		    double start_time = MAX(channel_idle_time_, NOW);
		    double end_time = MAX(channel_idle_time_, NOW+txtime);
		    double actual_txtime = end_time-start_time;

		    if (start_time > update_energy_time_) {
			    em()->DecrIdleEnergy(start_time - 
						 update_energy_time_, P_idle_);
			    update_energy_time_ = start_time;
		    }

		    /* It turns out that MAC sends packet even though, it's
		       receiving some packets.
		    
		    if (txtime-actual_txtime > 0.000001) {
			    fprintf(stderr,"Something may be wrong at MAC\n");
			    fprintf(stderr,"act_tx = %lf, tx = %lf\n", actual_txtime, txtime);
		    }
		    */

		   // Sanity check
		   double temp = MAX(NOW,last_send_time_);

		   /*
		   if (NOW < last_send_time_) {
			   fprintf(stderr,"Argggg !! Overlapping transmission. NOW %lf last %lf temp %lf\n", NOW, last_send_time_, temp);
		   }
		   */
		   
		   double begin_adjust_time = MIN(channel_idle_time_, temp);
		   double finish_adjust_time = MIN(channel_idle_time_, NOW+txtime);
		   double gap_adjust_time = finish_adjust_time - begin_adjust_time;
		   if (gap_adjust_time < 0.0) {
			   fprintf(stderr,"What the heck ! negative gap time.\n");
		   }

		   if ((gap_adjust_time > 0.0) && (status_ == RECV)) {
			   em()->DecrTxEnergy(gap_adjust_time,
					      Pt_consume_-Pr_consume_);
		   }

		   em()->DecrTxEnergy(actual_txtime,Pt_consume_);
//		   if (end_time > channel_idle_time_) {
//			   status_ = SEND;
//		   }
//
		   status_ = IDLE;

		   last_send_time_ = NOW+txtime;
		   channel_idle_time_ = end_time;
		   update_energy_time_ = end_time;

		   if (em()->energy() <= 0) {
			   em()->setenergy(0);
			   ((MobileNode*)node())->log_energy(0);
		   }

		} else {

			// log node energy
			if (em()->energy() > 0) {
				((MobileNode *)node_)->log_energy(1);
			} 
//
			Packet::free(p);
			return;
		}
	}

	/*
	 *  Stamp the packet with the interface arguments
	 */
	//NIST: add packet type
	//p->txinfo_.stamp((MobileNode*)node(), ant_->copy(), Pt_, lambda_); //old
	p->txinfo_.stamp((MobileNode*)node(), ant_->copy(), Pt_, lambda_, techno_);
	//end NIST

	// Send the packet
	channel_->recv(p, this);

        if (txqueued_) {
                purgeInterferenceQ();
                interfQ_[chan_].enque(p->copy());
        }
}

int 
WirelessPhy::sendUp(Packet *p)
{
	/*
	 * Sanity Check
	 */
	assert(initialized());

	PacketStamp s;
	double Pr;
	int pkt_recvd = 0;

	Pr = p->txinfo_.getTxPr();
	
	// if the node is in sleeping mode, drop the packet simply
	if (em()) {
			if (Is_node_on()!= true){
			pkt_recvd = 0;
			goto DONE;
			}

			if (Is_sleeping()==true && (Is_node_on() == true)) {
				pkt_recvd = 0;
				goto DONE;
			}
			
	}
	// if the energy goes to ZERO, drop the packet simply
	if (em()) {
		if (em()->energy() <= 0) {
			pkt_recvd = 0;
			goto DONE;
		}
	}

	if(propagation_) {
		//NIST: add packet type
		//s.stamp((MobileNode*)node(), ant_, 0, lambda_);
		s.stamp((MobileNode*)node(), ant_, 0, lambda_, techno_);
		//end NIST
		Pr = propagation_->Pr(&p->txinfo_, &s, this);
		//fprintf (stderr, "Packet info Pr=%e, CS=%e, RXThresh=%e\n", Pr, CSThresh_, RXThresh_); 

		if (Pr < CSThresh_) {
			pkt_recvd = 0;
			goto DONE;
		}
		if (Pr < RXThresh_) {
			/*
			 * We can detect, but not successfully receive
			 * this packet.
			 */
			hdr_cmn *hdr = HDR_CMN(p);
			hdr->error() = 1;
#if DEBUG > 3
			printf("SM %f.9 _%d_ drop pkt from %d low POWER %e/%e\n",
			       Scheduler::instance().clock(), node()->index(),
			       p->txinfo_.getNode()->index(),
			       Pr,RXThresh);
#endif
		}
	}
	if(modulation_) {
		hdr_cmn *hdr = HDR_CMN(p);
		hdr->error() = modulation_->BitError(Pr);
	}
	
	//NIST: add interference
	//now do interference
	if (isInterferenceEnabled()){
		//printf ("pktRx=%ld, p=%ld, expiration=%f, now=%f\n", (long)pktRx_, (long)p, expiration_, NOW);
		if (pktRx_) {
			if (expiration_ <= NOW) {
				pktRx_ = NULL;
			} else {
				//calculate interference of p onto pktRx_
				//arguments: receiver_type, receiver_freq, t_power, t_distance_rx_tx, interferer_type, interferer_freq, i_power, i_distance_rx
				/*
				  printf ("R: X=%.2f Y=%.2f, T: X=%.2f Y=%.2f, I: X=%.2f Y=%.2f\n", ((MobileNode*)node_)->X(),((MobileNode*)node_)->Y(),
				  pktRx_->txinfo_.getNode()->X(), pktRx_->txinfo_.getNode()->Y(),
				  p->txinfo_.getNode()->X(), p->txinfo_.getNode()->Y());			
				*/
				int nberr = Interference::interference((MobileNode*)node_, pktRx_, expiration_-HDR_CMN(pktRx_)->txtime(), p, NOW);
				int before= HDR_CMN(p)->error();
				HDR_CMN(pktRx_)->error() = nberr>0 || HDR_CMN(pktRx_)->error();
				if (nberr>0)
					printf ("%f nbErr=%d, packet size=%d, error=%d(before=%d)\n", NOW, nberr, HDR_CMN(pktRx_)->size(), HDR_CMN(pktRx_)->error(), before);
			
			}
		}
		//check if the packet needs to be forwarded to upper layer
		//for now, if not same techno, then drop.
		//if 802.11 then drop if frequency is different
		//printf ("receiver type=%d packet type=%d\n", techno_, p->txinfo_.getPktType());
		if (p->txinfo_.getPktType() != techno_ 
		    || (techno_ == WLAN_PKT_TYPE && p->txinfo_.getLambda()!=lambda_)) {
			//printf ("Drop packet %f %f \n",p->txinfo_.getLambda(),lambda_);
			pkt_recvd = 0;
			goto DONE;
		} 

		if (!HDR_CMN(p)->error()) {
			expiration_ = NOW+HDR_CMN(p)->txtime();
			pktRx_ = p;
		}
	}
	//end NIST

	/*
	 * The MAC layer must be notified of the packet reception
	 * now - ie; when the first bit has been detected - so that
	 * it can properly do Collision Avoidance / Detection.
	 */
	pkt_recvd = 1;

DONE:
	p->txinfo_.getAntenna()->release();

	/* WILD HACK: The following two variables are a wild hack.
	   They will go away in the next release...
	   They're used by the mac-802_11 object to determine
	   capture.  This will be moved into the net-if family of 
	   objects in the future. */
	p->txinfo_.RxPr = Pr;
	p->txinfo_.CPThresh = CPThresh_;

	/*
	 * Decrease energy if packet successfully received
	 */
	if(pkt_recvd && em()) {

		double rcvtime = hdr_cmn::access(p)->txtime();
		// no way to reach here if the energy level < 0
		
		double start_time = MAX(channel_idle_time_, NOW);
		double end_time = MAX(channel_idle_time_, NOW+rcvtime);
		double actual_rcvtime = end_time-start_time;

		if (start_time > update_energy_time_) {
			em()->DecrIdleEnergy(start_time-update_energy_time_,
					     P_idle_);
			update_energy_time_ = start_time;
		}
		
		em()->DecrRcvEnergy(actual_rcvtime,Pr_consume_);
/*
  if (end_time > channel_idle_time_) {
  status_ = RECV;
  }
*/
		channel_idle_time_ = end_time;
		update_energy_time_ = end_time;

		status_ = IDLE;

		/*
		  hdr_diff *dfh = HDR_DIFF(p);
		  printf("Node %d receives (%d, %d, %d) energy %lf.\n",
		  node()->address(), dfh->sender_id.addr_, 
		  dfh->sender_id.port_, dfh->pk_num, node()->energy());
		*/

		// log node energy
		if (em()->energy() > 0) {
		((MobileNode *)node_)->log_energy(1);
        	} 

		if (em()->energy() <= 0) {  
			// saying node died
			em()->setenergy(0);
			((MobileNode*)node())->log_energy(0);
		}
	}
	
	return pkt_recvd;
}

void
WirelessPhy::node_on()
{

        node_on_= TRUE;
	status_ = IDLE;

       if (em() == NULL)
 	    return;	
   	if (NOW > update_energy_time_) {
      	    update_energy_time_ = NOW;
   	}
}

void 
WirelessPhy::node_off()
{

        node_on_= FALSE;
	status_ = SLEEP;

	if (em() == NULL)
            return;
        if (NOW > update_energy_time_) {
            em()->DecrIdleEnergy(NOW-update_energy_time_,
                                P_idle_);
            update_energy_time_ = NOW;
	}
}

void 
WirelessPhy::node_wakeup()
{

	if (status_== IDLE)
		return;

	if (em() == NULL)
            return;

        if ( NOW > update_energy_time_ && (status_== SLEEP) ) {
		//the power consumption when radio goes from SLEEP mode to IDLE mode
		em()->DecrTransitionEnergy(T_transition_,P_transition_);
		
		em()->DecrSleepEnergy(NOW-update_energy_time_,
				      P_sleep_);
		status_ = IDLE;
	        update_energy_time_ = NOW;
		
		// log node energy
		if (em()->energy() > 0) {
			((MobileNode *)node_)->log_energy(1);
	        } else {
			((MobileNode *)node_)->log_energy(0);   
	        }
	}
}

void 
WirelessPhy::node_sleep()
{
//
//        node_on_= FALSE;
//
	if (status_== SLEEP)
		return;

	if (em() == NULL)
            return;

        if ( NOW > update_energy_time_ && (status_== IDLE) ) {
	//the power consumption when radio goes from IDLE mode to SLEEP mode
	    em()->DecrTransitionEnergy(T_transition_,P_transition_);

            em()->DecrIdleEnergy(NOW-update_energy_time_,
                                P_idle_);
		status_ = SLEEP;
	        update_energy_time_ = NOW;

	// log node energy
		if (em()->energy() > 0) {
			((MobileNode *)node_)->log_energy(1);
	        } else {
			((MobileNode *)node_)->log_energy(0);   
	        }
	}
}
//
void
WirelessPhy::dump(void) const
{
	Phy::dump();
	fprintf(stdout,
		"\tPt: %f, Gt: %f, Gr: %f, lambda: %f, L: %f\n",
		Pt_, ant_->getTxGain(0,0,0,lambda_), ant_->getRxGain(0,0,0,lambda_), lambda_, L_);
	//fprintf(stdout, "\tbandwidth: %f\n", bandwidth_);
	fprintf(stdout, "--------------------------------------------------\n");
}


void WirelessPhy::UpdateIdleEnergy()
{
	if (em() == NULL) {
		return;
	}
	if (NOW > update_energy_time_ && (Is_node_on()==TRUE && status_ == IDLE ) ) {
		  em()-> DecrIdleEnergy(NOW-update_energy_time_,
					P_idle_);
		  update_energy_time_ = NOW;
	}

	// log node energy
	if (em()->energy() > 0) {
		((MobileNode *)node_)->log_energy(1);
        } else {
		((MobileNode *)node_)->log_energy(0);   
        }

//	idle_timer_.resched(10.0);
}

double WirelessPhy::getDist(double Pr, double Pt, double Gt, double Gr,
			    double hr, double ht, double L, double lambda)
{
	if (propagation_) {
		return propagation_->getDist(Pr, Pt, Gt, Gr, hr, ht, L,
					     lambda);
	}
	return 0;
}

//
void WirelessPhy::UpdateSleepEnergy()
{
	if (em() == NULL) {
		return;
	}
	if (NOW > update_energy_time_ && ( Is_node_on()==TRUE  && Is_sleeping() == true) ) {
		  em()-> DecrSleepEnergy(NOW-update_energy_time_,
					P_sleep_);
		  update_energy_time_ = NOW;
		// log node energy
		if (em()->energy() > 0) {
			((MobileNode *)node_)->log_energy(1);
        	} else {
			((MobileNode *)node_)->log_energy(0);   
        	}
	}
	
	//A hack to make states consistent with those of in Energy Model for AF
	int static s=em()->sleep();
	if(em()->sleep()!=s){

		s=em()->sleep();	
		if(s==1)
			node_sleep();
		else
			node_wakeup();			
		printf("\n AF hack %d\n",em()->sleep());	
	}	
	
	sleep_timer_.resched(10.0);
}

void WirelessPhy::purgeInterferenceQ()
{
        double t = Scheduler::instance().clock();
        Packet *p;
        hdr_cmn *ch;
        while ((p = interfQ_[chan_].head())) {
                ch = HDR_CMN(p);
                if (t - (ch->ts_ + ch->txtime()) > MAXCOLLISIONWINDOW) {
			Packet::free(interfQ_[chan_].deque());
                } else {
                        return;
                }
        }
}
