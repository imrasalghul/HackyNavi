# Native user client through the actual accelerator port

The Metal frontend now opens its existing native buffer-graphics client on
the exact `HackyNaviAccelerator` port supplied by Metal discovery. It no longer
enumerates a second service and opens the parent device port. Standalone native
tools keep their direct `HackyNaviDevice` route. A failure opening the accelerator
is returned; there is no fallback to the direct route.

This implements a real user-client path, not successful target execution.
It does not make an unsupported Apple private accelerator client available or
change macOS security policy. Native compilation, target admission, and actual
GPU execution are distinct checks.

## Kernel ownership

The auxiliary kext remains the genuine `IOAccelerator` subclass. Its
`newUserClient` accepts only HackyNavi's exact `0x484e5631` client type and the
calling thread's real `current_task()`. Other types are rejected. An auxiliary
mutex protects the retained owner and stop state; it is released before any
provider callback, client startup, or engine operation.

A synchronous, versioned kernel-only `OpenRequest` is sent through the real
provider's `callPlatformFunction`. It contains the actual child, task and
IOKit-supplied creation parameters. There are no callbacks, imported project
C++ symbols, copied Apple clients, GPU addresses, or caller-created ownership
tokens in this contract. The core checks the exact class, real superclass,
direct provider and registry identity.

The core records `acceleratorGeneration_` when it retains and publishes the
actual accelerator. It clears the generation with that child on withdrawal
or retirement. Opening snapshots this private child/generation under the core
mutex; a property or request-supplied number cannot choose the generation.

The core's private factory initializes the existing `HackyNaviUserClient`,
binds it to retained real owner and accelerator objects, and attaches it to
that exact accelerator. Normal client startup calls `createClientSession`
once. That routine obtains one fresh FunctionGroup Operation and verifies the
same published child, private publication generation, current Operation
generation, actual provider, power and revocation state before and after the
real lifecycle creates its native session. The open forwarding method holds
no outer Operation, avoiding recursive acquisition of that gate.

The session owns the same real engine/GART/queue as the existing direct route
and gets its normal isolated per-client VM. It does not acquire another PCI
owner or manufacture another hardware epoch. Every subsequent native request
continues to validate the session's actual FunctionGroup generation.

Shutdown can overlap startup. A newly created session is installed into the
user client only while its state mutex still shows open admission; a concurrent
close cannot be followed by installation of another live session. Failed or
revoked candidates are closed and released only after the creation Operation
and GART scopes have ended. Attaching the user client to the actual accelerator
also gives IOKit the correct provider/client termination relationship.

## User-space identity

`Client::openBufferedGraphicsAccelerator` takes a borrowed accelerator port and
both expected registry IDs. It explicitly retains the port, verifies the exact
class and real IOAccelerator ancestry, reads its immediate real
HackyNaviDevice parent, and checks the owner-ID property for consistency.
It performs one `IOServiceOpen` on that accelerator port. Hello is negotiated
against the expected native owner ID, then the same port and immediate parent
relationship are checked again before returning the connection.

The two IDs keep their existing meanings:

| Identity | Meaning |
| --- | --- |
| Metal device `registryID` | Actual accelerator service registry ID |
| Native Hello `registryId` | Actual HackyNaviDevice hardware-owner registry ID |
| Native Hello epoch | Kernel-issued per-client session identity |
| Private publication generation | Current FunctionGroup generation bound to the retained accelerator |

The private publication generation never becomes a user-space authorization
token. The native wire protocol and Hello layout are unchanged. The Context
adds an explicit accelerator open route, while the Metal initializer changes
only its binding call and continues to validate the exact owner relationship
and capabilities.

## Validation scope

`build/accelerator-port-route-local-1/report.json` records strict compilation
of four changed kernel translation units against real kernel headers with a
2 KiB frame limit, plus an x86_64 Darwin Context object. Actual object symbol
tables are checked to keep IOAccelerator imports out of the early core and
project C++ imports out of the auxiliary bundle. IOKit transport and Objective-C
frontend compilation require the native macOS SDK. No mocked IOKit service,
kernel load, Metal enumeration, or GPU execution supplies this evidence.
