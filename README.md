#[Title]: CXP
#[Author]: Dimitris Mavrommatis (mavromat@ics.forth.gr) -- @inspire_forth

------------------------------------------------------------------------------------------------------------

#[Details]: 
To run this project you have to put the CXP.py file inside the /pox/ext directory and then place the cxp
folder which contains the configuration file inside the /pox/ directory.

Then you need to connect to the remote VMs and run as sudo the relay_scripts/server.py script which has to
be in the same directory as the server.out file that is compiled from the server.c file. Running the 
server.py script is opening a port at 32033 on eth0 interface and waits for the CXP controller to connect and
start the communication.

After running the server.py on all of the remote VMs you will have to create a configuration file named
servers.json inside the cxp folder. For example having two VMs with alias VM1 and VM2 and ips 10.10.10.1 and
20.20.20.1 the configuration file should be like this:

[ 
	{ 
		"name":"VM1",
		"ip":"10.10.10.1" 
	},
	{
		"name":"VM2",
		"ip":"20.20.20.1"
	}
]

Then you can run the module from inside pox by running ./pox.py CXP. You will notice that the Controller
connects to all the remote VMs and initializes the OVS switches while creating aswell the GRE tunnels
between them. Every X minutes the controller is going to request for the VMs to run the server.out program
which is going to calculate actively the one way delay between the VMs and store all the information at
the controller.

The controller can then create a Weighted BiDirectional Graph with one way delays as weights and do path
stiching depending on the lowest latency path.

------------------------------------------------------------------------------------------------------------

#[Warning]:
This script comes as-is with no promise of functionality or accuracy. I did not write it to be efficient nor 
secured. Feel free to change or improve it any way you see fit.

------------------------------------------------------------------------------------------------------------   

#[Modification, Distribution, and Attribution]:
You are free to modify and/or distribute this script as you wish.  I only ask that you maintain original
author attribution and not attempt to sell it or incorporate it into any commercial offering.
