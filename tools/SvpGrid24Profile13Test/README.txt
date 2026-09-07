MPCVR SVP4 grid24 + active-profile shader13 TEST

Double-click Install-Test.cmd with MPC-HC closed. The script requests elevation,
keeps the same pre-grid24 rollback copy created by the previous test installer,
installs both x86 and x64 payloads, and verifies their SHA-256 hashes after copying.

Restore-Previous.cmd restores the renderer saved before the grid24 experiments.

This test keeps the grid24 / 1/6 vec_src motion path, but now matches the active
SVP profile's fi_shader=13 and exact recovered algorithm-13 temporal median rule.
It is a research TEST, not Build 4.
