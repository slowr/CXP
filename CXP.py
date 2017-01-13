#!/usr/bin/python

###############################################################################################################
## [Title]: CXP.py -- CXP pox controller
## [Author]: Dimitris Mavrommatis (mavromat@ics.forth.gr) -- @inspire_forth
##-------------------------------------------------------------------------------------------------------------
## [Details]: 
## You need to run this module from inside pox/ext/ folder. The controller's job is to initialize all the VMs
## that are inside the servers.json file and create GRE tunnels in between them. Every X minutes you can confi-
## gure the controller to request one way delays of all the VMs and build a Directional Weighted Graph. The
## Graph then can be used to path stich the lowest latency path.
##
## For example:
## The one way delay from VM1 to VM3 might be 50ms but the path through VM2 might be 10ms less. So the controller
## will path stich the forward path to be VM1 -> VM2 -> VM3. For the return path it is going to see the one way
## delays as well and decied which is the lowest latency path.
##-------------------------------------------------------------------------------------------------------------
## [Warning]:
## This script comes as-is with no promise of functionality or accuracy. I did not write it to be efficient nor 
## secured. Feel free to change or improve it any way you see fit.
##-------------------------------------------------------------------------------------------------------------   
## [Modification, Distribution, and Attribution]:
## You are free to modify and/or distribute this script as you wish.  I only ask that you maintain original
## author attribution.
###############################################################################################################


# Generic imports
import sys
import os
import random
import time
import traceback
import csv
import socket
import simplejson as json
import signal
import thread
import fcntl
import struct
import datetime

# Pox-specific imports
from pox.core import core
from pox.openflow import ethernet
import pox.openflow.libopenflow_01 as of
from pox.lib.packet.arp import arp
from pox.lib.addresses import EthAddr, IPAddr
from pox.lib.revent import *
from pox.lib.recoco import Timer
from pox.lib.util import dpidToStr

import subprocess

# Networkx import for graph management
import networkx as nx

# For beautiful prints of dicts, lists, etc,
from pprint import pprint as pp

log = core.getLogger()

bridge2ip = {}      # key: name, value: tunnel ip
servers = []        # (name,public ip,tunnel ip)

class CXP(EventMixin):

    _neededComponents = set([])

    def __init__ (self):
        super(EventMixin, self).__init__()
        self.dpid2switch = {}       # key: DPID, value: Tunneled Switch
        self.arpmap = {}            # key: IP address, value: Tunneled Switch
        self.G = nx.DiGraph()
        
        self.check_directories()

        # This thread receives the one way delays from the other nodes
        thread.start_new_thread(self.start_delay_controller,())

        # Every X period we request the nodes to recalculate the one way delays
        thread.start_new_thread(self.precise_calculate_delays,())

        # Controller will send bash commands to setup OVS interfaces on the remote nodes
        self.initialize_servers()

        # Add all the nodes to a Directional Graph 
        for s in bridge2ip:
            self.G.add_node( s )

        # Invoke event listeners
        if not core.listen_to_dependencies(self, self._neededComponents):
            self.listenTo(core)
        self.listenTo(core.openflow)

    def get_ip_address(self, ifname):
        '''
            Get the IP address of the ifname interface.
        '''
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        return socket.inet_ntoa(fcntl.ioctl(
            s.fileno(),
            0x8915,  # SIOCGIFADDR
            struct.pack('256s', ifname[:15])
        )[20:24])

    def precise_calculate_delays(self):
        '''
            Run calculate_delays function every 6 minutes.
        '''
        while(1):
            a = datetime.datetime.now()
            if (a.minute % 6) == 0:
                self.calculate_delays()
                Timer(360, self.calculate_delays, recurring = True)
                break
            time.sleep(1)
            #print a.minute
        print "Calculating delays started at " + time.strftime("%c")

    def check_directories(self):
        ''' 
            Check if directories exist.
        '''

        if not os.path.exists('./cxp/'):
            os.makedirs('cxp')

        if not os.path.exists('./cxp/delays/'):
            os.makedirs('./cxp/delays')

        if not os.path.exists('./cxp/logs/'):
            os.makedirs('./cxp/logs')

    def start_delay_controller (self):
        '''
            Receiving socket for the one way delays of the remote nodes. We save the delays on a specified folder
            to use them later on the construction of the Directional Graph.
        '''

        # Create a TCP/IP socket
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        # Bind the socket to the port
        server_address = (self.get_ip_address('eth0'), 32032)
        sock.bind(server_address)

        log.info("Delay controller up and running..")
        while True:
            data, address = sock.recvfrom(4096)
            ss = data.split(" ")

            print "Received data %s from %s" %(data,address)
            if len(ss[0]) > 2:
                # Save the latest delay on a file.
                with open('./cxp/delays/'+ss[0],'w') as f:
                    f.write(str(data.split("end",1)[0]).replace(ss[0]+" ",""))

                # Append the latest delay on a logs file.
                with open('./cxp/logs/'+ss[0],'a') as f:
                    f.write( time.strftime("%c \t") + " " + str(data.split("end",1)[0]).replace(ss[0] + " ","") + "\n")
        log.info("Closing delay controller..")

    def initialize_servers (self):
        '''
            Setup OVS interfaces for each node by sending the bash commands the node will run.
        '''
        log.info("Initialize OVS on server side..")

        if os.path.exists('./cxp/servers.json'):
            with open('./cxp/servers.json','r') as f:
                data = json.load(f)
                i = 100
                for VM in data:
                    servers.append( (VM['name'],VM['ip'],str('192.168.10.'+str(i)))  )
                    bridge2ip[VM['name']] = IPAddr(str('192.168.10.'+str(i)))
                    i += 1
        else:
            log.error('There is no servers.json file in the cxp directory. Please create a configuration file before continuing..')
            os.exit(-1)

        def add_command_to_list(all_commands, command):
            c_list = []
            for word in command.split():
                c_list.append(word)
            all_commands.append(c_list)

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Send the crafted bash commands to each node.
        for server in servers:
            all_commands = ['s']

            server_name = server[0]
            send_ip = server[1]
            ip_id = server[2]
            
            command = str("ovs-vsctl --if-exists del-br %s" %(server_name))
            add_command_to_list(all_commands,command)

            # Add the OVS bridge.
            command = str("ovs-vsctl add-br %s" %(server_name))
            add_command_to_list(all_commands,command)   

            # Assign the private IP to the bridge that we are going to assign to the GRE tunnel.
            command = str("ifconfig %s %s/24" %(server_name,ip_id))
            add_command_to_list(all_commands,command)

            # For each remote node add a GRE tunnel.
            for k in servers:
                if server != k:
                    remote_server = k[0]
                    remote_ip = k[1]
                    command = str("ovs-vsctl add-port %s %s -- set interface %s type=gre options:remote_ip=%s" %(server_name,remote_server,remote_server,remote_ip))
                    add_command_to_list(all_commands,command)

            # Connect the OVS bridge to the main controller.
            command = str("ovs-vsctl set-controller %s tcp:%s:6633" %(server_name,self.get_ip_address('eth0'))) 
            add_command_to_list(all_commands,command)

            server_address = (send_ip, 32033) 
            sent = sock.sendto(json.dumps(all_commands), server_address)
        sock.close()

    def calculate_delays (self):
        '''
            Send a request to all remote nodes to run one way delay measurements to each other.
        '''
        log.info("Requesting delays...")
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        for s in servers:
            server_address = (s[1], 32033)
            new_servers = ['d',s[0]]
            for k in servers:
                if k != s: new_servers.append(k)
            # Send data
            sent = sock.sendto(json.dumps(new_servers), server_address)
        sock.close()

    def calculate_best_paths (self):
        '''
            Setup a Directional Graph with one way delays as edge weights.
        '''
        tmp = nx.DiGraph()
        tmp.add_nodes_from(self.G.nodes())
        # Delete outdated edges
        self.G.clear()
        self.G.add_nodes_from(tmp)

        for server in bridge2ip:
            fname = './cxp/logs/%s' % server
            print "Reading delays for %s bridge" % (fname)

            if os.path.exists(fname):
                with open(fname,'r') as f:
                    ss = f.readline().split(" ")
                    for i in range(0,len(ss)-1):
                        sm = ss[i].split(":")
                        self.G.add_edge(str(server), sm[0], weight=float(sm[1]) )
                


    def _handle_ConnectionUp (self, event):
        log.info("Switch %s has come up.", dpidToStr(event.dpid))
        if event.dpid not in self.dpid2switch:
            self.dpid2switch[event.dpid] = TunnelSwitch()
            self.dpid2switch[event.dpid].connect(event.connection) 
            self.arpmap[self.dpid2switch[event.dpid].ip_addr] = self.dpid2switch[event.dpid]
        if len(self.dpid2switch) == len(bridge2ip):
            log.info("All relay nodes connected.. Learning topology.")
            for tswitch in self.dpid2switch:
                self.learn_macs(self.dpid2switch[tswitch])

    def learn_macs(self, tswitch):
        for port_no in tswitch.ports:
            arp_request = arp()
            arp_request.hwsrc = tswitch.hw_addr
            arp_request.hwdst = EthAddr("FF:FF:FF:FF:FF:FF")
            arp_request.opcode = arp.REQUEST
            arp_request.protosrc = tswitch.ip_addr
            arp_request.protodst = tswitch.ports[port_no][0]
            ether = ethernet()
            ether.type = ethernet.ARP_TYPE
            ether.dst = EthAddr("FF:FF:FF:FF:FF:FF")
            ether.src = tswitch.hw_addr
            ether.payload = arp_request
            msg = of.ofp_packet_out(in_port=of.OFPP_NONE)
            msg.actions.append(of.ofp_action_output(port = port_no))
            msg.data = ether
            tswitch.connection.send(msg)

    def _handle_PacketIn(self, event):
        packet = event.parsed
        dpid = event.dpid
        inport = event.port

        def handle_ARP_pktin():
            srcip = IPAddr(packet.next.protosrc)
            dstip = IPAddr(packet.next.protodst)
            if packet.next.opcode == arp.REQUEST:
                log.debug("Handling ARP packet: %s requests the MAC of %s" % (str(srcip), str(dstip)))
                if dstip in self.arpmap.keys():
                    self.arpmap[dstip].send_arp_reply( packet, inport )
            elif packet.next.opcode == arp.REPLY:
                log.debug("Handling ARP packet: %s responds to %s" % (str(srcip), str(dstip)))
            else:
                log.debug("Unknown ARP type")
                return
            return

        def handle_IP_pktin():
            srcip = IPAddr(packet.next.srcip)
            dstip = IPAddr(packet.next.dstip)
            src_mac = packet.src
            dst_mac = packet.dst

            log.debug("Handling IP packet between %s and %s" % (str(srcip), str(dstip)))

            if srcip in self.arpmap.keys() and dstip in self.arpmap.keys():
                self.calculate_best_paths()
                log.info("%s -> %s" % (self.arpmap[dstip].bridge, self.arpmap[srcip].bridge))

                # We find the shortest weight path from the Directional Graph.
                starttime = time.time()
                path = list(nx.all_shortest_paths(self.G, self.arpmap[dstip].bridge, self.arpmap[srcip].bridge, weight='weight'))
                endtime = time.time()

                log.info(str(path) + " - time elapsed for path calculation: " + str(endtime-starttime))
                
                for s in range(0,len(path[0])-1):
                    self.arpmap[ bridge2ip[ path[0][s] ] ].install_flow_rule( dstip, srcip, self.arpmap[bridge2ip[path[0][s+1]]] )
                self.arpmap[ bridge2ip[ path[0][s+1] ] ].install_flow_rule( dstip, srcip, self.arpmap[bridge2ip[path[0][s+1]]], True )

                log.info("%s -> %s" % (self.arpmap[srcip].bridge, self.arpmap[dstip].bridge))

                starttime = time.time()
                path = list(nx.all_shortest_paths(self.G, self.arpmap[srcip].bridge, self.arpmap[dstip].bridge, weight='weight'))
                endtime = time.time()

                log.info(str(path) + " - time elapsed for path calculation: " + str(endtime-starttime))
                
                for s in range(0,len(path[0])-1):
                    self.arpmap[ bridge2ip[ path[0][s] ] ].install_flow_rule( srcip, dstip, self.arpmap[bridge2ip[path[0][s+1]]] )
                self.arpmap[ bridge2ip[ path[0][s+1] ] ].install_flow_rule( srcip, dstip, self.arpmap[bridge2ip[path[0][s+1]]], True )
            return

        #--------------------------------------------------------------------------------------------------------------
        if packet.type == packet.LLDP_TYPE:
            return

        elif packet.type == packet.ARP_TYPE:
            handle_ARP_pktin()
            return

        elif packet.type == packet.IP_TYPE:
            handle_IP_pktin()
            return

        else:
            #log.info("Unknown Packet type: %s" % packet.type)
            return


class TunnelSwitch (EventMixin):

    def __init__ (self):
        self.bridge = None      # name of bridge on specified switch
        self.ip_addr = None     # tunneled ip address
        self.hw_addr = None     # mac address of the switch
        self.connection = None 
        self.dpid = None
        self.ports = {}         # key = port_no, value = (remote host IP, local gre macs)
        self._listeners = None
        self.ip2port = {}       # key = IP, value = outport

    def __repr__(self):
        return dpidToStr(self.dpid)

    def connect(self, connection):
        self.dpid = connection.dpid
        self.hw_addr = EthAddr(dpidToStr(connection.dpid).replace('-',':'))
        for k in connection.features.ports:
            if k.port_no != 65534: 
                self.ports[k.port_no] = ( bridge2ip[port_name], EthAddr(k.hw_addr) )
                self.ip2port[bridge2ip[port_name]] = k.port_no
            else: 
                self.bridge = port_name
                self.ip_addr = bridge2ip[port_name]
                self.ip2port[bridge2ip[port_name]] = k.port_no
        self.connection = connection
        self._listeners = self.listenTo(connection)

    def send_arp_reply(self, packet, outport):      
        arp_reply = arp()
        arp_reply.hwsrc = self.hw_addr
        arp_reply.hwdst = packet.src
        arp_reply.opcode = arp.REPLY
        arp_reply.protosrc = packet.next.protodst
        arp_reply.protodst = packet.next.protosrc
        ether = ethernet()
        ether.type = ethernet.ARP_TYPE
        ether.dst = packet.src
        ether.src = self.hw_addr
        ether.payload = arp_reply
        msg = of.ofp_packet_out(in_port=of.OFPP_NONE)
        msg.actions.append(of.ofp_action_output(port = outport))
        msg.data = ether
        self.connection.send(msg)
        log.debug("Sendind ARP reply through port %s" % (outport))
        log.debug(arp_reply)

    def install_flow_rule(self, srcip, dstip, next_hop_switch, last_node=False):
        log.debug("install_flow_rule src %s dst %s brg %s outport %s" %(srcip, dstip, self.bridge, self.ip2port[next_hop_switch.ip_addr]))
        msg = of.ofp_flow_mod()
        msg.match.dl_type = 0x800
        msg.match.nw_src = srcip
        msg.match.nw_dst = dstip
        msg.actions.append(of.ofp_action_dl_addr.set_dst(next_hop_switch.hw_addr))
        msg.actions.append(of.ofp_action_output(port = self.ip2port[next_hop_switch.ip_addr]))
        msg.idle_timeout = 30
        msg.hard_timeout = 300
        self.connection.send(msg)

        if last_node:
            msg = of.ofp_flow_mod()
            msg.match.dl_type = 0x800
            msg.match.nw_src = srcip
            msg.match.nw_dst = dstip
            msg.actions.append(of.ofp_action_output(port = of.OFPP_LOCAL))
            msg.idle_timeout = 30
            msg.hard_timeout = 300
            self.connection.send(msg)

def signal_handler(signal, frame):
    '''
        When we kill the controller we clear the OVS interfaces on
        the remote nodes.
    '''
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for s in servers:
        server_address = (s[1], 32033)
        sent = sock.sendto(json.dumps(str('c ' + s[0])), server_address)
    sock.close()
    sys.exit(0)

def launch ():
    signal.signal(signal.SIGINT, signal_handler)
    core.registerNew(CXP)
