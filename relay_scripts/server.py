#!/usr/bin/python

###############################################################################################################
## [Title]: server.py -- a server script to cooperate with the CXP controller on the remote nodes.
## [Author]: Dimitris Mavrommatis (mavromat@ics.forth.gr) -- @inspire_forth
##-------------------------------------------------------------------------------------------------------------
## [Details]: 
## This script is intended to be run on Virtual Machines that have OVS installed as sudo. The CXP controller
## by modifying the servers.json file will try to connect to the VM and set up the OVS switch and run the 
## experiments we have configured for it.
##-------------------------------------------------------------------------------------------------------------
## [Warning]:
## This script comes as-is with no promise of functionality or accuracy. I did not write it to be efficient nor 
## secured. Feel free to change or improve it any way you see fit.
##-------------------------------------------------------------------------------------------------------------   
## [Modification, Distribution, and Attribution]:
## You are free to modify and/or distribute this script as you wish.  I only ask that you maintain original
## author attribution.
###############################################################################################################

import socket, sys, simplejson, subprocess , fcntl, struct, thread, time, threading, os
lock = threading.Lock()

# List of ripe probe IPs to run traceroutes to.
ripe_nodes = []

def get_ip_address(ifname):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    return socket.inet_ntoa(fcntl.ioctl(
        s.fileno(),
        0x8915,  # SIOCGIFADDR
        struct.pack('256s', ifname[:15])
    )[20:24])

def write_to_file_ping(host):
	'''
		Write the rtt of the pings to a file.
	'''
	ping_response = subprocess.Popen(["/bin/ping", "-qc10", str(host)], stdout=subprocess.PIPE)
	beautify_response = subprocess.check_output(('grep', 'rtt'), stdin=ping_response.stdout)
	lock.acquire()
	with open('./logs/ripe','a+') as f:
		f.write(time.strftime("%c") +'\t' + str(host) + '\t' + str(beautify_response) + '\n')
	lock.release()

def write_to_file_traceroute(host):
	'''
		Write traceroutes to a file and remove unanswered lines.
	'''
	ping_response = subprocess.Popen(["traceroute", str(host)], stdout=subprocess.PIPE)
	beautify_response = subprocess.check_output(('grep','-vw','* * *'), stdin=ping_response.stdout)
	lock.acquire()
	with open('./logs/traceroute','a+') as f:
		f.write(time.strftime("%c") +'\t' + str(host) + '\t' + str(beautify_response) + '\n')
	lock.release()

def run_pings():
	'''
		Run pings on specified ripe nodes.
	'''
	for ripe in ripe_nodes:
		print "running ping to %s" % (ripe)
		thread.start_new_thread(write_to_file_ping,(ripe,))

def run_traceroutes(do_nodes):
	'''
		Run traceroutes on specified ripe nodes and remote nodes.
	'''
	for ripe in ripe_nodes:
		print "running traceroute to %s" % (ripe)
		thread.start_new_thread(write_to_file_traceroute,(ripe,))

	for node in do_nodes:
		print "running traceroute to %s" % (node)
		thread.start_new_thread(write_to_file_traceroute,(node,))
		
def main():
	# Create a TCP/IP socket
	sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

	# Bind the socket to the port
	if len(sys.argv) > 1:
		server_address = (get_ip_address(sys.argv[1]), 32033)
	else:
		server_address = (get_ip_address('eth0'), 32033)
	sock.bind(server_address)

	if not os.path.exists('./logs'):
		os.makedirs('./logs')

	while True:
		data, address = sock.recvfrom(4096)
		data = simplejson.loads(data)

		print "received data %s from %s" % (data, address)

		# Option d: 
		# We run pings and traceroutes to specified RIPE and remote nodes to log them. Also, server.out is going
		# to calculate the oneway delays and inform the controller with the results.
		if data[0] == 'd':
			thread.start_new_thread(run_pings,())
			tmp = ""
			do_nodes = []
			for s in range(2,len(data)):
				tmp += str(data[s][0]) + ":"  + str(data[s][1]) + "|"
				do_nodes.append(str(data[s][1]));
			tmp = tmp[:-1]
			thread.start_new_thread(run_traceroutes,(do_nodes,))
			print "calling ./server.out " + str(len(data)-2) + " " + str(data[1]) + " \"" + str(tmp) + "\" " + address[0]
			subprocess.call(["./server.out", str(len(data)-2), str(data[1]), str(tmp), address[0]])

		# Option s:
		# We execute each bash command that is sent from the controller in order to setup the OVS bridge.
		elif data[0] == 's':
			for command in data:
				if str(command) == 's': continue
				if 'ovs-vsctl' or 'ifconfig' in command:
					print "calling " + str(command)
					subprocess.call(command)

		# Option c:
		# When controller is killed a custom packet is sent to clear the OVS bridge on the remote nodes.
		elif data[0] == 'c':
			ss = str(data).split()
			print ss
			print "calling " + "ovs-vsctl " + "--if-exists " + "del-br " + ss[1]
			subprocess.call(["ovs-vsctl","--if-exists","del-br",ss[1]])

if __name__ == "__main__":
	if os.getuid() == 0:
   		main()
   	else:
   		print "Please run script with sudo.."
