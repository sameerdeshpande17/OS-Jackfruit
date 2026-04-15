 Supervised Multi-Container Runtime with Kernel Memory Monitor
Name: Satwik Inchageri & Sameer Deshpande
SRN: PES1UG24CS618  & PES1UG24CS610
Section: K
________________________________________
📌 1. Project Overview
This project implements a lightweight container runtime in C along with a Linux kernel module to monitor and enforce memory limits per container.
The system demonstrates key OS concepts:
• Process isolation using namespaces
• Kernel-user interaction via ioctl
• Memory management and enforcement
• CPU scheduling behavior
________________________________________
🧱 System Architecture
🔹 User Space (engine.c)
• Supervisor process
• CLI interface
• Container lifecycle management
• Logging system (bounded buffer)
🔹 Kernel Space (monitor.c)
• Tracks container processes
• Monitors RSS memory usage
• Enforces soft and hard memory limits
________________________________________
⚙️ 2. Build, Load and Run Instructions
🔧 Build
cd boilerplate
make
________________________________________
🔧 Load Kernel Module
sudo insmod monitor.ko
Verify:
ls -l /dev/container_monitor
________________________________________
🔧 Start Supervisor
sudo ./engine supervisor ../rootfs-base
________________________________________
🔧 Prepare Root Filesystems
cp -a ../rootfs-base ../rootfs-alpha
cp -a ../rootfs-base ../rootfs-beta
________________________________________
🔧 Copy Workloads
cp cpu_hog ../rootfs-alpha/
cp cpu_hog ../rootfs-beta/
cp memory_hog ../rootfs-beta/
cp io_pulse ../rootfs-alpha/

chmod +x ../rootfs-alpha/*
chmod +x ../rootfs-beta/*
________________________________________
🔧 Run Containers
sudo ./engine start alpha ../rootfs-alpha "/cpu_hog 180"
sudo ./engine start beta ../rootfs-beta "/memory_hog 8 500"
________________________________________
🔧 View Containers
sudo ./engine ps
________________________________________
🔧 View Logs
sudo ./engine logs alpha
________________________________________
🔧 Stop Containers
sudo ./engine stop alpha
sudo ./engine stop beta
________________________________________
🔧 Kernel Logs
dmesg | tail
________________________________________
🔧 Cleanup
sudo rmmod monitor
________________________________________
📸 3. Demo with Screenshots
________________________________________
1. Multi-Container Supervision
Two containers running under one supervisor.
 
 
 
________________________________________

2. Metadata Tracking
Output of engine ps.
 


________________________________________
3. Bounded Buffer Logging
Logs captured via logging pipeline.
 ________________________________________
4. CLI and IPC
Command issued + supervisor response.
 
________________________________________
5. Soft Limit Warning
Kernel log showing warning.
 ________________________________________
6. Hard Limit Enforcement
Process killed after exceeding memory.
 ________________________________________
7. Scheduling Experiment
Difference between nice values.
  ________________________________________
8. Clean Teardown
No leftover processes.
 ________________________________________
🧠 4. Engineering Analysis
🔹 Process Isolation
Containers use Linux namespaces (PID, mount, UTS) to isolate processes and filesystem.
________________________________________
🔹 Memory Monitoring
Kernel module periodically checks RSS (Resident Set Size) of processes using:
• get_mm_rss()
________________________________________
🔹 Soft vs Hard Limits
Type Behavior
Soft Warning only
Hard Process killed
________________________________________
🔹 Kernel-User Communication
Implemented using ioctl system calls through:
/dev/container_monitor
________________________________________
🔹 Scheduling Behavior
Linux scheduler uses nice values to determine priority.
Lower nice → higher priority → more CPU time.
________________________________________
⚖️ 5. Design Decisions & Tradeoffs
🔹 Namespace Isolation
• Used clone() with namespaces
• Tradeoff: more complexity vs strong isolation
________________________________________
🔹 Supervisor Architecture
• Single supervisor managing all containers
• Tradeoff: central control vs single point of failure
________________________________________
🔹 IPC Mechanism
• UNIX domain socket
• Tradeoff: simplicity vs scalability
________________________________________
🔹 Logging System
• Bounded buffer (producer-consumer)
• Tradeoff: limited buffer vs controlled memory usage
________________________________________
🔹 Kernel Monitor
• Timer-based monitoring
• Tradeoff: periodic check vs real-time overhead
________________________________________
📊 6. Scheduler Experiment Results
🔹 Experiment Setup
Container Nice Value Workload
c1 0 cpu_hog
c2 10 cpu_hog
________________________________________
🔹 Observation
• c1 progresses faster
• c2 progresses slower
________________________________________
🔹 Conclusion
Lower nice value → higher CPU priority → faster execution
Higher nice value → lower priority → slower execution
________________________________________
📈 7. Observations
• Memory limits enforced correctly
• Logging system stable under load
• I/O workloads remain responsive
• CPU scheduling clearly visible
________________________________________
🚀 8. Conclusion
This project successfully demonstrates:
• Container runtime design
• Kernel module development
• Memory monitoring and enforcement
• Scheduling analysis
