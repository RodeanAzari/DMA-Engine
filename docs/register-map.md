<!-- GENERATED - DO NOT EDIT. Source: spec/dmaeng.yaml -->
# Register map (BAR0)

| Offset | Name | Access | Reset | Description |
|---|---|---|---|---|
| `0x00` | `ID` | RO | `0x444D4131` | Magic value, driver probe sanity check |
| `0x04` | `VERSION` | RO | `0x10000` | Version, major.minor |
| `0x08` | `CTRL` | RW | `0x0` | Control |
| `0x0C` | `STATUS` | RO | `0x0` | Engine status |
| `0x10` | `RING_BASE_LO` | RW | `0x0` | Ring physical address, low 32 bits |
| `0x14` | `RING_BASE_HI` | RW | `0x0` | Ring physical address, high 32 bits |
| `0x18` | `RING_SIZE` | RW | `0x0` | Descriptor count, power of two, 2 to 4096 |
| `0x1C` | `HEAD` | RO | `0x0` | Device consume index |
| `0x20` | `TAIL` | RW | `0x0` | Driver produce index, a write rings the doorbell |
| `0x24` | `IRQ_STATUS` | RW1C | `0x0` | Interrupt status, write 1 to clear |
| `0x28` | `IRQ_MASK` | RW | `0x0` | Interrupt mask, 1 = enabled |
| `0x2C` | `ERR_CODE` | RO | `0x0` | Last error code, see error_codes |
| `0x30` | `MAX_XFER` | RO | `0x10000` | Maximum bytes per descriptor, larger is rejected with error 0x1 |

## Register fields

| Register | Bit | Name | Description |
|---|---|---|---|
| `CTRL` | 0 | `ENABLE` | Enable the engine |
| `CTRL` | 1 | `RESET` | Reset the engine, self-clearing |
| `STATUS` | 0 | `BUSY` | Engine is processing descriptors |
| `STATUS` | 1 | `ERROR` | Engine stopped on an error, cleared by CTRL.RESET |
| `IRQ_STATUS` | 0 | `COMPLETION` | A descriptor with DESC_IRQ completed |
| `IRQ_STATUS` | 1 | `ERROR` | The device hit an error |
| `IRQ_MASK` | 0 | `COMPLETION` | Enable completion interrupt |
| `IRQ_MASK` | 1 | `ERROR` | Enable error interrupt |

## Descriptor (32 bytes, little-endian)

| Offset | Name | Width (bytes) | Description |
|---|---|---|---|
| `0x00` | `src` | 8 | Source physical address |
| `0x08` | `dst` | 8 | Destination physical address |
| `0x10` | `len` | 4 | Transfer length in bytes |
| `0x14` | `flags` | 4 | See flags |
| `0x18` | `status` | 4 | Written by the device on completion, before OWN is cleared |
| `0x1C` | `rsvd` | 4 | Must be zero |

### Descriptor flags

| Bit | Name | Description |
|---|---|---|
| 0 | `OWN` | 1 = device owns the descriptor, device clears on completion |
| 1 | `IRQ` | Raise completion interrupt when this descriptor finishes |
| 2 | `LAST` | Last descriptor in a logical batch, informational |

## Error codes

| Code | Name | Description |
|---|---|---|
| `0x0` | `OK` | Success |
| `0x1` | `BAD_LEN` | Invalid length, zero or exceeds MAX_XFER |
| `0x2` | `DMA_FAIL` | Address translation failure, DMA access to guest memory failed |
| `0x3` | `BAD_DESC` | Descriptor malformed, rsvd non-zero or misaligned ring |
