# SwiftBaton Artifact Evaluation

Artifact for **SwiftBaton: Dependency-Aware Staged Reconstruction for Live Migration of Stateful Containers**, accepted at **ACM ATC 2026**.

SwiftBaton studies dependency-aware, staged reconstruction for live migration of stateful containers. Its design combines sub-state scheduling, page scheduling, and coordinated page completion to reduce service interruption and accelerate throughput recovery.

> **Preparation status:** This README is a draft. The release, reviewer access arrangements, and complete reproduction instructions are being prepared. This repository is not yet a complete executable artifact. Items marked **TBD** must be completed before this document is used as the final AE guide.

## 1. Artifact overview and scope

The planned artifact consists of the SwiftBaton implementation, workload configurations, experiment drivers, raw measurements, and scripts that generate the corresponding figures and tables.

The final release will identify the experiments and paper claims supported by the packaged implementation. Development runs have exercised a basic Redis migration workflow, but those runs alone do not establish reproduction of the paper's performance results or validation of both SwiftBaton-U and SwiftBaton-K.

**TBD:** Add an inventory of the files actually included in the release and map each supported paper figure/table to its experiment and analysis commands. State unsupported claims explicitly.

## 2. Obtain the artifact

- Artifact homepage: [sirius0118/SwiftBaton-AE](https://github.com/sirius0118/SwiftBaton-AE).
- Evaluation release and exact commit: **TBD**.
- Source code, dependency packages, and reference data downloads: **TBD**.
- Final archival DOI: **TBD — to be added after the final version is archived**.

Use the version identified above when comparing results. Record the commit and configuration with each run.

## 3. Obtain access to the experiment environment

The development environment uses a self-hosted bare-metal Linux cluster with separate workload-client, migration-source, and migration-destination nodes. Live migration requires RDMA-capable networking, custom host software, and privileges to perform checkpoint/restore and configure networking.

**TBD:** Confirm whether author-provided machines will be available to reviewers, and document the access request channel, availability period, account setup, and experiment scheduling procedure. If SSH access is provided, arrange it through the AE submission's private discussion channel without requiring reviewers to reveal their identities. Do not publish passwords or private keys.

For reproduction on independently provisioned machines, the final release must include host setup and build instructions in addition to workload container images. A workload Docker image alone does not capture the required host kernel, runtime, and RDMA configuration.

## 4. Prepare and check the environment

The following components have been observed in the existing development environment; the final AE machine configuration and package versions still need to be frozen:

| Component | Existing development setup |
| --- | --- |
| Topology | Three nodes: client, source, destination |
| Source/destination kernel | Linux 5.15.167 |
| Migration stack | Modified CRIU, Docker/containerd/runc, RDMA support, and a page-access collection kernel module |
| Workload used in the basic migration check | Redis and modified YCSB |
| Supporting tools | Python 3, Java 8, Maven, SSH, rsync, SSHFS, iptables, conntrack, and redis-cli |

**TBD:** Provide the exact AE CPU, RAM, NIC, OS, driver versions, bandwidth configuration, package/build identifiers, and container image digests. Document required privileges, disk space, and installation time. Add commands to build/install the software and verify kernel-module, RDMA, runtime, and peer-connectivity prerequisites.

## 5. Run a minimal working example

The initial workflow is: start Redis on the source, load records, generate a sustained workload, checkpoint and restore the container, transfer pages, redirect client traffic, verify destination service and data, and confirm that the source is no longer required.

**TBD:** Package and document a tested command for this workflow, its working directory, configuration file, expected duration, expected output, and result location. Include explicit success/failure criteria and instructions for safely cleaning up only the resources created by the run.

Until the runner is made configurable, run only one migration experiment at a time; the current development runner uses shared ports and coordination paths.

## 6. Reproduce the supported paper experiments

**TBD:** For each supported figure/table, provide the input parameters, baseline and SwiftBaton variants, exact run command, repetition count, resource requirements, expected execution time, raw-output path, and expected qualitative result. Explain any differences between the paper environment and the supplied AE environment.

Report observed results without replacing missing samples with synthetic values. Distinguish client-observed service interruption from internal checkpoint/restore timing. Measure TTR from the start of service interruption, consistently with the paper.

## 7. Analyze results and clean up

**TBD:** Provide commands that turn the collected raw logs into CSV files and paper-style plots/tables. Include reference outputs and explain expected variation. Document how to preserve results and remove the experiment's containers, mounts, processes, and network rules after a run or failure.

## 8. Support and licensing

- During artifact evaluation: use the private discussion channel on the AE submission site.
- Public contact and post-evaluation support: **TBD**.
- License and third-party notices: **TBD — preserve upstream licenses and identify the license for author-created material before the final release**.
