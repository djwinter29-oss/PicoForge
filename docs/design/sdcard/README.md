# SD-Card Documentation

These documents separate SD-card driver design from the filesystem performance-test procedure:

| Document | Scope and authority |
| --- | --- |
| [SD-card driver design](sdcard-driver-design.md) | Driver layers and interfaces, protocol workflows, ownership, resource use, and validation limits. |
| [SD-card performance test](sdcard-performance-test.md) | Test safety, execution, measurements, and result interpretation. |

Physical wiring, transport engines, and board resources are maintained in the
[SD-card driver guide](../../../firmware/src/drivers/sdcard/README.md). Filesystem commands and mounting policy
belong to the [USB CDC CLI guide](../usb/usb-cdc-cli.md). Dated hardware measurements and review history are in
[test records](../../tests/records/README.md); current review dispositions are in the
[SD remediation matrix](../../tests/sd-design-remediation.md).

For the broader firmware layer and task model, see the [system architecture](../architecture.md).
