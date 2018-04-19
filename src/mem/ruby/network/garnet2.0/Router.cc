/*
 * Copyright (c) 2008 Princeton University
 * Copyright (c) 2016 Georgia Institute of Technology
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Niket Agarwal
 *          Tushar Krishna
 */


#include "mem/ruby/network/garnet2.0/Router.hh"

#include "base/stl_helpers.hh"
#include "debug/RubyNetwork.hh"
#include "mem/ruby/network/garnet2.0/CreditLink.hh"
#include "mem/ruby/network/garnet2.0/CrossbarSwitch.hh"
#include "mem/ruby/network/garnet2.0/GarnetNetwork.hh"
#include "mem/ruby/network/garnet2.0/InputUnit.hh"
#include "mem/ruby/network/garnet2.0/NetworkLink.hh"
#include "mem/ruby/network/garnet2.0/OutputUnit.hh"
#include "mem/ruby/network/garnet2.0/RoutingUnit.hh"
#include "mem/ruby/network/garnet2.0/SwitchAllocator.hh"

using namespace std;
using m5::stl_helpers::deletePointers;

Router::Router(const Params *p)
    : BasicRouter(p), Consumer(this)
{
    m_latency = p->latency;
    m_virtual_networks = p->virt_nets;
    m_vc_per_vnet = p->vcs_per_vnet;
    m_num_vcs = m_virtual_networks * m_vc_per_vnet;

    // initializing swizzleSwap parameters
    inport_occupancy = 0;
    is_critical = false;
    critical_inport.id = -1;
    critical_inport.dirn = "Unknown";
    critical_inport.send_credit = false;

    m_routing_unit = new RoutingUnit(this);
    m_sw_alloc = new SwitchAllocator(this);
    m_switch = new CrossbarSwitch(this);

    m_input_unit.clear();
    m_output_unit.clear();
}

Router::~Router()
{
    deletePointers(m_input_unit);
    deletePointers(m_output_unit);
    delete m_routing_unit;
    delete m_sw_alloc;
    delete m_switch;
}

void
Router::init()
{
    BasicRouter::init();

    // initialize your critical inport here
    // and then put an assert in the wakeup
    // that its never disabled and never points
    // to "Local_"
    for(int inport=0; inport < m_input_unit.size(); inport++) {
        if(m_input_unit[inport]->get_direction() != "Local") {
            critical_inport.id = inport;
            critical_inport.dirn = m_input_unit[inport]->get_direction();
            assert(m_input_unit[inport]->vc_isEmpty(0) == true);
            break;
        }
    }
    // initialize outVcState of upstream router as well...
    Router* router_ = get_net_ptr()->\
            get_RouterInDirn(getInportDirection(critical_inport.id), m_id);
    // get outputUnit direction in router_
    PortDirection upstream_outputUnit_dirn =
            input_output_dirn_map(getInportDirection(critical_inport.id));
    // get id for that direction in router_
    int upstream_outputUnit_id =
            router_->m_routing_unit->m_outports_dirn2idx[upstream_outputUnit_dirn];

    router_->get_outputUnit_ref()[upstream_outputUnit_id]->set_vc_critical(0, true);

    m_sw_alloc->init();
    m_switch->init();
}

void
Router::critical_swap(int critical_inport_id, int inport_id)
{
    // critical_inport_id must be empty
    // inport_id must NOT be empty... doSwap
    // doSwap.. just set the inport-vc active and idle accordingly..
    flit *t_flit;
    t_flit =  m_input_unit[inport_id]->getTopFlit(0);
    // insert the flit..
    m_input_unit[critical_inport_id]->insertFlit(0,t_flit);

    // update credit for both upstream routers...
    // increment credit for upstream router for inport_id's outVC
    Router* router1 = get_net_ptr()->\
                get_RouterInDirn(getInportDirection(inport_id), m_id);
    // get outputUnit direction in router1
    PortDirection upstream1_outputUnit_dirn =
                input_output_dirn_map(getInportDirection(inport_id));
    // get id for that direction in router1
    int upstream1_outputUnit = router1->m_routing_unit->\
                        m_outports_dirn2idx[upstream1_outputUnit_dirn];
    router1->get_outputUnit_ref()[upstream1_outputUnit]->\
                                        increment_credit(0);
    m_input_unit[inport_id]->set_vc_idle(0, curCycle()); // set this vc idle
    // update the outVcState for this router as well.. make it IDLE_
    router1->get_outputUnit_ref()[upstream1_outputUnit]->\
                           set_vc_state(IDLE_, 0, curCycle());
    // mark the critical outVc in upstream tourter as well...
    router1->get_outputUnit_ref()[upstream1_outputUnit]->\
                            set_vc_critical(0, true);
    /*-------------------------------------------------------------------------*/
    // decrement credit for upstream router for critical_inport_id's outVC
    Router* router2 = get_net_ptr()->\
            get_RouterInDirn(getInportDirection(critical_inport_id), m_id);
    // get outputUnit direction in router2
    PortDirection upstream2_outputUnit_dirn =
            input_output_dirn_map(getInportDirection(critical_inport_id));
    // get id for that direction in router2
    int upstream2_outputUnit = router2->m_routing_unit->\
                            m_outports_dirn2idx[upstream2_outputUnit_dirn];
    router2->get_outputUnit_ref()[upstream2_outputUnit]->\
                                        decrement_credit(0);
    //set vc state to be active
    m_input_unit[critical_inport_id]->set_vc_active(0, curCycle()); // set this vc active
    router2->get_outputUnit_ref()[upstream2_outputUnit]->\
                                set_vc_state(ACTIVE_, 0, curCycle());
    // unmark the outVc accordingly
    router2->get_outputUnit_ref()[upstream2_outputUnit]->\
                                set_vc_critical(0, false);

    // update the critical inport structure here...
    critical_inport.id = inport_id;
    critical_inport.dirn = getInportDirection(inport_id);

    return;
}


// this api will loop through all the input port of the router (point by my_id);
// except critical and local inport
bool
Router::chk_critical_deflect(int my_id)
{
    std::vector<int> doDeflect;
    // 2 Local_ ports and 1 critical port
    // doDeflect.resize(m_input_unit.size() - 3);
    Router* router; // this is the router...
    for(int inport = 0; inport < m_input_unit.size(); ++inport) {
        // 2 Local_ direction
        if(getInportDirection(inport) == "Local")
            continue;
        // 1 critical port
        if(inport == critical_inport.id)
            continue;

        // populate doDeflect vector
        router = get_net_ptr()->get_RouterInDirn(getInportDirection(inport), my_id);
        // calculate occupancy of this router...
        std::vector<InputUnit *> input_unit_ = router->get_inputUnit_ref();
        int router_occupancy = 0;
        for(int input_port = 0; input_port < input_unit_.size(); ++input_port) {
            // calculate the occupancy of this router
            if(input_unit_[input_port]->vc_isEmpty(0) == false) {

                router_occupancy++;
            }
        }
        // after this loop if router_occupancy == N-2 then add the bit in 'doDeflect'
        // vector.
        if(router_occupancy == (input_unit_.size() - 2)) {
            cout << "increasing the space of deflection vector"\
                " for doing bubble deflection" << endl;
            doDeflect.push_back(1);
        }
    }
    // if the size of 'doDeflect' vector is N-3 then perform critical-bubble deflection.
    if(doDeflect.size() >= (m_input_unit.size() - 3)) {
        cout << "<<<<<<<< Initiate the critical bubble delect sequence >>>>>>>>" << endl;
        return true;
    } else {
        cout << "doDeflect.size(): " << doDeflect.size() << " and (m_input_unit.size() - 3) "\
            << (m_input_unit.size() - 3) << endl;
        cout << "therefore can't initiate the bubble deflection sequence "<< endl;
        return false;
    }

}

void
Router::wakeup()
{
    DPRINTF(RubyNetwork, "Router %d woke up\n", m_id);
    cout << "Router-" << m_id << " woke up" << endl;
    if(get_net_ptr()->isEnableSwizzleSwap() == true) {
        assert(m_input_unit[critical_inport.id]->vc_isEmpty(0) == true);
        assert(m_input_unit[critical_inport.id]->get_direction() != "Local");
        assert(m_input_unit[critical_inport.id]->get_direction() ==
            critical_inport.dirn);
        Router* router_ = get_net_ptr()->
            get_RouterInDirn(getInportDirection(critical_inport.id), m_id);
        PortDirection upstream_outputUnit_dirn =
            input_output_dirn_map(getInportDirection(critical_inport.id));
        int upstream_outputUnit_id =
            router_->m_routing_unit->\
                    m_outports_dirn2idx[upstream_outputUnit_dirn];
        assert(router_->get_outputUnit_ref()[upstream_outputUnit_id]->\
                                        is_vc_critical(0) == true);
        // assert that there's only one critical outVc among connecting routers...
        int critical_vc_cnt = 0;
        for(int inp_=0; inp_< m_input_unit.size(); ++inp_) {

            if(getInportDirection(inp_) == "Local")
                continue;

            Router* routr_ = get_net_ptr()->
                get_RouterInDirn(getInportDirection(inp_), m_id);
            PortDirection upstream_outputUnit_dirn =
                input_output_dirn_map(getInportDirection(inp_));
            int upstream_outputUnit_id =
                routr_->m_routing_unit->m_outports_dirn2idx[upstream_outputUnit_dirn];
            if(routr_->get_outputUnit_ref()[upstream_outputUnit_id]->is_vc_critical(0)
                == true) {
                critical_vc_cnt++;
                cout << "critical_vc_cnt: " << critical_vc_cnt << " from router-id: "\
                    <<routr_->get_id()<< endl;
            }
        }
        cout << "critical_vc_cnt: " << critical_vc_cnt << endl;
        assert(critical_vc_cnt == 1);
    }

    int router_occupancy = 0;
    // check for incoming flits
    for (int inport = 0; inport < m_input_unit.size(); inport++) {
        m_input_unit[inport]->wakeup();
        if (m_input_unit[inport]->vc_isEmpty(0) == false)
            router_occupancy++;
    }

    // Now all packets in the input port has been put from the links...
    // do the swizzleSwap here with differnt options..
    // next option is to always keep doing swap instead of doing at TDM_
    // if((curCycle()%(get_net_ptr()->getNumRouters())) == m_id) {
    // We will swap everytime the router wakesup()
    if(get_net_ptr()->isEnableSwizzleSwap()) {
        // option-1: Minimal
        if(get_net_ptr()->getPolicy() == MINIMAL_) {
            int success = -1;
            success = swapInport();

            if(success == 1)
                cout << "Swap completed with empty input-port..." << endl;
            else if (success == 2)
                cout << "Swap completed with flit with differnt outport"<< endl;
            else if (success == 0)
                cout << "Swap couldn't be completed..." << endl;
            else
                fatal("Not possible \n");

            // at TDM_ if router's all inport are occupied.. deflect..
            // exchange bubbles..
            // Critical-Deflect_
            if((curCycle() == get_net_ptr()->prnt_cycle)) {
                get_net_ptr()->prnt_cycle += 103;
                get_net_ptr()->scanNetwork();
            }
            cout << "router_occupancy: "<< router_occupancy << " inputUnit.size(): "\
                << m_input_unit.size() << " (m_input_unit.size()-2): " << (m_input_unit.size()-2)\
                << endl;
            if(((curCycle()%(get_net_ptr()->getNumRouters())) == m_id) &&
                (router_occupancy == (m_input_unit.size()-2))) {
                // check the occupancy at the router pointed by each outport
                // of the flit present in this router..
                bool doCriticalDeflect = false;
                doCriticalDeflect = chk_critical_deflect(m_id);
                if(doCriticalDeflect == true) {
                    cout << "do bubble deflection " << endl;
                } else {
                    cout << "don't do bubble deflection " << endl;
                }

            }
        } // option-2: Non-Minimal
        else if(get_net_ptr()->getPolicy() == NON_MINIMAL_) {
            fatal("Not implemented \n"); // implement deflection here
        }
    }
    // }
    // check for incoming credits
    // Note: the credit update is happening before SA
    // buffer turnaround time =
    //     credit traversal (1-cycle) + SA (1-cycle) + Link Traversal (1-cycle)
    // if we want the credit update to take place after SA, this loop should
    // be moved after the SA request
    for (int outport = 0; outport < m_output_unit.size(); outport++) {
        m_output_unit[outport]->wakeup();
    }

    // Switch Allocation
    m_sw_alloc->wakeup();

    // Switch Traversal
    m_switch->wakeup();

    // calculate inport occupancy of the router...
    // reset:
    inport_occupancy = 0;
    for (int inport = 0; inport < m_input_unit.size(); inport++) {
        if (m_input_unit[inport]->vc_isEmpty(0) == false)
            inport_occupancy++;
    }

}


// Rules for swap:
// 0. choose the inport randomly to swap from..
// 1. if there's an empty inport present then first swap from there..
//      then take care of credit signalling in the outVC state of
//      respective routers
// 2. else swap with the filled ones and then you don't need to take
//     care of the credit signalling...

// Note: implement peekTopFlit on linkClass as well.. if there's already
// packet sitting on the link then don't decrement credits for the bubble...
int
Router::swapInport() {
    // two inport-id from where we need to swap
    int inport_full = -1;
    int inport_empty = -1;

    // you already have a critical inport.. just need to find another inport
    // to swap this critical inport..
    // 1. It should not be Local_
    // 2. If it's empty then make sure there is nothing on the link
    // 3. or it could be non-empty (in which case use critical_swap()-api)
    int itr_cnt = 0;
    while(1) {
        itr_cnt++;
        if(itr_cnt >= 100)
            return 0; // unsuccessful
        // choose a random inport
        int inport_ = random() % (m_input_unit.size());
        // do not swap if its local_
        if(getInportDirection(inport_) == "Local")
            continue;

        // either empty -- break;
        if((m_input_unit[inport_]->vc_isEmpty(0) == true) &&
            (getInportDirection(inport_) != "Local") &&
            (inport_ != critical_inport.id) ) {

            Router* router2 = get_net_ptr()->\
                get_RouterInDirn(getInportDirection(inport_), m_id);
            PortDirection upstream2_outputUnit_dirn = input_output_dirn_map(getInportDirection(inport_));
            int upstream2_outputUnit = router2->m_routing_unit->m_outports_dirn2idx[upstream2_outputUnit_dirn];
            if (router2->get_outputUnit_ref()[upstream2_outputUnit]->is_vc_idle(0, curCycle()) == false)
                continue; // not possible so break... instread of continue

            inport_empty = inport_;
            break;
        }

        // Or filled -- break;
        if((m_input_unit[inport_]->vc_isEmpty(0) == false) &&
            (getInportDirection(inport_) != "Local") &&
            (inport_ != critical_inport.id)) {
            inport_full = inport_;
            break;
        }
    }

    assert(((inport_empty != -1) && (inport_full == -1)) ||
            ((inport_empty == -1) && (inport_full != -1)));

    // doShuffle...
    if(inport_empty == -1) {
        //swap between 'input_full' and 'critical id'
        critical_swap(critical_inport.id, inport_full);
    }
    else if(inport_full == -1) {

        Router* router1 = get_net_ptr()->\
            get_RouterInDirn(getInportDirection(inport_empty), m_id);
        // get outputUnit direction in router1
        PortDirection upstream1_outputUnit_dirn =
                input_output_dirn_map(getInportDirection(inport_empty));
        // get id for that direction in router1
        int upstream1_outputUnit =
                router1->m_routing_unit->\
                m_outports_dirn2idx[upstream1_outputUnit_dirn];
        // mark the critical outVc in upstream tourter as well...
        router1->get_outputUnit_ref()[upstream1_outputUnit]->\
                                        set_vc_critical(0, true);
        /*-------------------------------------------------------------------------*/
        // decrement credit for upstream router for critical_inport_id's outVC
        Router* router2 = get_net_ptr()->\
                get_RouterInDirn(getInportDirection(critical_inport.id), m_id);
        // get outputUnit direction in router2
        PortDirection upstream2_outputUnit_dirn =
                input_output_dirn_map(getInportDirection(critical_inport.id));
        // get id for that direction in router2
        int upstream2_outputUnit =
                router2->m_routing_unit->\
                m_outports_dirn2idx[upstream2_outputUnit_dirn];
        // unmark the outVc accordingly
        router2->get_outputUnit_ref()[upstream2_outputUnit]->\
                                        set_vc_critical(0, false);

        // update the critical inport structure here...
        critical_inport.id = inport_empty;
        critical_inport.dirn = getInportDirection(inport_empty);

    }

    #if(MY_PRINT)
        cout << "--------------------------" << endl;
        cout << "Router-id:  " << m_id << "  Cycle: " << curCycle() << endl;
        cout << "--------------------------" << endl;
    #endif
    if ((inport_full != -1) && (inport_empty != -1)) {
        #if(MY_PRINT)
            cout << "Non-Empty inport is: " << inport_full << " direction: " << getInportDirection(inport_full) << endl;
            cout << "flit present: " << *m_input_unit[inport_full]->peekTopFlit(0) << endl;
            cout << "Empty inport is: " << inport_empty << " direction: " << getInportDirection(inport_empty) << endl;
        #endif
        critical_swap(inport_empty, inport_full);
        return 1;
    }

    return 0;
}


void
Router::addInPort(PortDirection inport_dirn,
                  NetworkLink *in_link, CreditLink *credit_link)
{
    int port_num = m_input_unit.size();
    InputUnit *input_unit = new InputUnit(port_num, inport_dirn, this);

    input_unit->set_in_link(in_link);
    input_unit->set_credit_link(credit_link);
    in_link->setLinkConsumer(this);
    credit_link->setSourceQueue(input_unit->getCreditQueue());

    m_input_unit.push_back(input_unit);

    m_routing_unit->addInDirection(inport_dirn, port_num);
}

void
Router::addOutPort(PortDirection outport_dirn,
                   NetworkLink *out_link,
                   const NetDest& routing_table_entry, int link_weight,
                   CreditLink *credit_link)
{
    int port_num = m_output_unit.size();
    OutputUnit *output_unit = new OutputUnit(port_num, outport_dirn, this);

    output_unit->set_out_link(out_link);
    output_unit->set_credit_link(credit_link);
    credit_link->setLinkConsumer(this);
    out_link->setSourceQueue(output_unit->getOutQueue());

    m_output_unit.push_back(output_unit);

    m_routing_unit->addRoute(routing_table_entry);
    m_routing_unit->addWeight(link_weight);
    m_routing_unit->addOutDirection(outport_dirn, port_num);
}

PortDirection
Router::getOutportDirection(int outport)
{
    return m_output_unit[outport]->get_direction();
}

PortDirection
Router::getInportDirection(int inport)
{
    return m_input_unit[inport]->get_direction();
}

int
Router::route_compute(RouteInfo route, int inport, PortDirection inport_dirn)
{
    return m_routing_unit->outportCompute(route, inport, inport_dirn);
}

void
Router::grant_switch(int inport, flit *t_flit)
{
    m_switch->update_sw_winner(inport, t_flit);
}

void
Router::schedule_wakeup(Cycles time)
{
    // wake up after time cycles
    scheduleEvent(time);
}

std::string
Router::getPortDirectionName(PortDirection direction)
{
    // PortDirection is actually a string
    // If not, then this function should add a switch
    // statement to convert direction to a string
    // that can be printed out
    return direction;
}

void
Router::regStats()
{
    BasicRouter::regStats();

    m_buffer_reads
        .name(name() + ".buffer_reads")
        .flags(Stats::nozero)
    ;

    m_buffer_writes
        .name(name() + ".buffer_writes")
        .flags(Stats::nozero)
    ;

    m_crossbar_activity
        .name(name() + ".crossbar_activity")
        .flags(Stats::nozero)
    ;

    m_sw_input_arbiter_activity
        .name(name() + ".sw_input_arbiter_activity")
        .flags(Stats::nozero)
    ;

    m_sw_output_arbiter_activity
        .name(name() + ".sw_output_arbiter_activity")
        .flags(Stats::nozero)
    ;
}

void
Router::collateStats()
{
    for (int j = 0; j < m_virtual_networks; j++) {
        for (int i = 0; i < m_input_unit.size(); i++) {
            m_buffer_reads += m_input_unit[i]->get_buf_read_activity(j);
            m_buffer_writes += m_input_unit[i]->get_buf_write_activity(j);
        }
    }

    m_sw_input_arbiter_activity = m_sw_alloc->get_input_arbiter_activity();
    m_sw_output_arbiter_activity = m_sw_alloc->get_output_arbiter_activity();
    m_crossbar_activity = m_switch->get_crossbar_activity();
}

void
Router::resetStats()
{
    for (int j = 0; j < m_virtual_networks; j++) {
        for (int i = 0; i < m_input_unit.size(); i++) {
            m_input_unit[i]->resetStats();
        }
    }

    m_switch->resetStats();
    m_sw_alloc->resetStats();
}

void
Router::printFaultVector(ostream& out)
{
    int temperature_celcius = BASELINE_TEMPERATURE_CELCIUS;
    int num_fault_types = m_network_ptr->fault_model->number_of_fault_types;
    float fault_vector[num_fault_types];
    get_fault_vector(temperature_celcius, fault_vector);
    out << "Router-" << m_id << " fault vector: " << endl;
    for (int fault_type_index = 0; fault_type_index < num_fault_types;
         fault_type_index++) {
        out << " - probability of (";
        out <<
        m_network_ptr->fault_model->fault_type_to_string(fault_type_index);
        out << ") = ";
        out << fault_vector[fault_type_index] << endl;
    }
}

void
Router::printAggregateFaultProbability(std::ostream& out)
{
    int temperature_celcius = BASELINE_TEMPERATURE_CELCIUS;
    float aggregate_fault_prob;
    get_aggregate_fault_probability(temperature_celcius,
                                    &aggregate_fault_prob);
    out << "Router-" << m_id << " fault probability: ";
    out << aggregate_fault_prob << endl;
}

uint32_t
Router::functionalWrite(Packet *pkt)
{
    uint32_t num_functional_writes = 0;
    num_functional_writes += m_switch->functionalWrite(pkt);

    for (uint32_t i = 0; i < m_input_unit.size(); i++) {
        num_functional_writes += m_input_unit[i]->functionalWrite(pkt);
    }

    for (uint32_t i = 0; i < m_output_unit.size(); i++) {
        num_functional_writes += m_output_unit[i]->functionalWrite(pkt);
    }

    return num_functional_writes;
}

Router *
GarnetRouterParams::create()
{
    return new Router(this);
}
