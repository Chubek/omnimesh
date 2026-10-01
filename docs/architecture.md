# OmniMesh architecture

The scaffold separates control plane (desired state, admission, scheduling and allocation) from execution plane (node agents, workers, resources and monitoring). Supporting services, runtimes, extensions and common interfaces remain independently replaceable.

Invariants: unsupported capabilities are rejected or reported explicitly; desired and observed state are distinct; plugin ABI calls return statuses and never leak exceptions; resource reservations do not imply execution.
