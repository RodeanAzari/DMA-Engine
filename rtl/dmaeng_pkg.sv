// GENERATED - DO NOT EDIT. Source: spec/dmaeng.yaml
package dmaeng_pkg;

  localparam int unsigned ID_MAGIC = 32'h444D4131;
  localparam int unsigned VERSION = 32'h10000;
  localparam int unsigned RING_SIZE_MIN = 32'h2;
  localparam int unsigned RING_SIZE_MAX = 32'h1000;
  localparam int unsigned MAX_XFER_DEFAULT = 32'h10000;
  localparam int unsigned RING_ALIGN = 32'h1000;
  localparam int unsigned DESC_SIZE = 32'h20;
  localparam int unsigned REG_ACCESS_BYTES = 32'h4;

  localparam logic [11:0] REG_ID = 12'h000;
  localparam logic [11:0] REG_VERSION = 12'h004;
  localparam logic [11:0] REG_CTRL = 12'h008;
  localparam logic [11:0] REG_STATUS = 12'h00C;
  localparam logic [11:0] REG_RING_BASE_LO = 12'h010;
  localparam logic [11:0] REG_RING_BASE_HI = 12'h014;
  localparam logic [11:0] REG_RING_SIZE = 12'h018;
  localparam logic [11:0] REG_HEAD = 12'h01C;
  localparam logic [11:0] REG_TAIL = 12'h020;
  localparam logic [11:0] REG_IRQ_STATUS = 12'h024;
  localparam logic [11:0] REG_IRQ_MASK = 12'h028;
  localparam logic [11:0] REG_ERR_CODE = 12'h02C;
  localparam logic [11:0] REG_MAX_XFER = 12'h030;

  localparam int CTRL_ENABLE_BIT = 0;
  localparam int CTRL_RESET_BIT = 1;
  localparam int STATUS_BUSY_BIT = 0;
  localparam int STATUS_ERROR_BIT = 1;
  localparam int IRQ_STATUS_COMPLETION_BIT = 0;
  localparam int IRQ_STATUS_ERROR_BIT = 1;
  localparam int IRQ_MASK_COMPLETION_BIT = 0;
  localparam int IRQ_MASK_ERROR_BIT = 1;

  localparam int DESC_OWN_BIT = 0;
  localparam int DESC_IRQ_BIT = 1;
  localparam int DESC_LAST_BIT = 2;

  localparam logic [3:0] ERR_OK = 4'h0;
  localparam logic [3:0] ERR_BAD_LEN = 4'h1;
  localparam logic [3:0] ERR_DMA_FAIL = 4'h2;
  localparam logic [3:0] ERR_BAD_DESC = 4'h3;

endpackage
