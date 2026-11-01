-- ParaMesh frames in Wireshark (M6-4): the 36-byte header and the payload of every opcode, as
-- docs/PROTOCOL.md defines them. Discovery on UDP 47000, daemon control on TCP 47001, and the
-- job data plane, on ports chosen at run time, found by the header's magic. The local link is
-- a Unix socket and does not reach a capture; its messages are decoded all the same.
--
-- Install: copy this file to ~/.local/lib/wireshark/plugins/ (or pass -X lua_script:FILE to
-- tshark). Filter with "pmsh", "pmsh.opcode == 0x30", "pmsh.job == 65537" and so on.

local pmsh = Proto("pmsh", "ParaMesh")

local OPCODES = {
  [0x01] = "HELLO", [0x02] = "BYE",
  [0x10] = "SPAWN_REQ", [0x11] = "SPAWN_OK", [0x12] = "LEDGER_SYNC", [0x13] = "LEAVE_INTENT",
  [0x14] = "SPAWN_DECLINE",
  [0x20] = "JOIN_JOB", [0x21] = "SEG_MAP", [0x22] = "HEARTBEAT", [0x23] = "JOB_END",
  [0x24] = "SEG_MAP_ACK",
  [0x30] = "READ_REQ", [0x31] = "WRITE_REQ", [0x32] = "UPGRADE_REQ", [0x38] = "READ_DATA",
  [0x39] = "WRITE_GRANT", [0x3A] = "UPGRADE_GRANT", [0x3B] = "REDIRECT", [0x3C] = "BUSY_RETRY",
  [0x40] = "INV", [0x41] = "INV_ACK", [0x42] = "FETCH", [0x43] = "FETCH_INV",
  [0x44] = "FETCH_DATA", [0x45] = "WRITEBACK", [0x46] = "WRITEBACK_ACK",
  [0x50] = "TASK_REQ", [0x51] = "TASK_ASSIGN", [0x52] = "NO_TASK", [0x53] = "TASK_DONE",
  [0x60] = "LOCK_ACQ", [0x61] = "LOCK_GRANT", [0x62] = "LOCK_REL", [0x63] = "BARRIER_ENTER",
  [0x64] = "BARRIER_RELEASE", [0x65] = "ATOMIC_OP", [0x66] = "ATOMIC_RESULT",
  [0x70] = "SEG_MIGRATE", [0x71] = "SEG_MIGRATE_DONE", [0x72] = "LEAVE_DONE",
  [0x73] = "REHOME_HINT",
  [0x80] = "L_REGISTER", [0x81] = "L_ADMIT_REQ", [0x82] = "L_ADMIT_OK", [0x83] = "L_ADMIT_REFUSED",
  [0x84] = "L_CHUNK", [0x85] = "L_SEGMENTS", [0x86] = "L_JOB_END", [0x87] = "L_STATS",
  [0x90] = "L_QUOTA", [0x91] = "L_LEAVE", [0x92] = "L_ABORT", [0x93] = "L_LEAVE_INTENT",
  [0x94] = "L_MEMBER",
  [0xA0] = "L_RUN_REQ", [0xA1] = "L_RUN_OK", [0xA2] = "L_RUN_REFUSED", [0xA3] = "L_LEAVE_REQ",
  [0xA4] = "L_LEAVE_REPLY",
}

local STATUS = {
  [0] = "OK", "LEFT", "NODE_LOST", "TIMEOUT", "TASK_FAILED", "BINARY_MISMATCH", "PROTOCOL",
  "UNSUPPORTED", "USER_ABORT", "INTERNAL", "PMD_LOST", "NO_CAPACITY", "DECLINED", "LEAVING",
  "BAD_HANDLE", "NOT_FOUND", "START_TIMEOUT",
}

local FLAGS = { [0x0001] = "ZERO_PAGE", [0x0002] = "COMPRESSED", [0x0004] = "READ_ONLY",
                [0x0008] = "RETRY", [0x0010] = "BASE_KEPT" }

local f = {
  magic = ProtoField.uint32("pmsh.magic", "Magic", base.HEX),
  version = ProtoField.uint8("pmsh.version", "Version"),
  opcode = ProtoField.uint8("pmsh.opcode", "Opcode", base.HEX, OPCODES),
  flags = ProtoField.uint16("pmsh.flags", "Flags", base.HEX),
  job = ProtoField.uint32("pmsh.job", "Job"),
  src = ProtoField.uint16("pmsh.src", "Source node"),
  dst = ProtoField.uint16("pmsh.dst", "Destination node"),
  req = ProtoField.uint64("pmsh.req", "Request ID"),
  epoch = ProtoField.uint32("pmsh.epoch", "Epoch"),
  len = ProtoField.uint32("pmsh.len", "Payload length"),
  crc = ProtoField.uint32("pmsh.crc", "Payload CRC-32C", base.HEX),
  page = ProtoField.uint64("pmsh.page", "Page ID"),
}
pmsh.fields = f

local malformed = ProtoExpert.new("pmsh.malformed", "Malformed ParaMesh payload",
                                  expert.group.MALFORMED, expert.severity.ERROR)
pmsh.experts = { malformed }

local MAGIC = 0x504D5348
local HEADER = 36

-- Payload layouts. Each field is { name, kind }:
--   u8 u16 u32 u64, ipv4, status, hash (32 bytes), str16, rsv (n bytes, given as third value),
--   bytes (a count of bytes given by an earlier field), str16s (that many str16),
--   array (that many entries of a sub-layout), page (a page ID, and 4,096 bytes unless the
--   header says ZERO_PAGE; with COMPRESSED, the encoded page of PROTOCOL.md 7.2), migrate (SEG_MIGRATE's entries), u32s (n counts).
local MEMBER = { {"node", "u16"}, {"slot", "u8"}, {"flags", "u8"}, {"addr", "ipv4"},
                 {"port", "u16"}, {"", "rsv", 2}, {"ram_weight", "u64"} }
local TOTAL = { {"peer", "u16"}, {"", "rsv", 2}, {"chunks_for_peer", "u64"},
                {"chunks_by_peer", "u64"}, {"segsec_for_peer", "u64"}, {"segsec_by_peer", "u64"} }
local PAGE_ID = { {"page_id", "pageid"} }
local PAGE = { {"page", "page"} }
local STATUS_TEXT = { {"status", "status"}, {"message", "str16"} }
local END = { {"status", "status"}, {"node", "u16"}, {"message", "str16"} }
local HANDLE_THREAD = function(name) return { {name, "u64"}, {"thread", "u16"}, {"", "rsv", 6} } end
local HANDLE_STATUS = function(name) return { {name, "u64"}, {"status", "status"}, {"", "rsv", 6} } end

local LAYOUT = {
  [0x01] = { {"instance", "u32"}, {"control_port", "u16"}, {"flags", "u16"}, {"cap_ram", "u64"},
             {"free_ram", "u64"}, {"spill_budget", "u64"}, {"free_spill", "u64"},
             {"cap_cores", "u16"}, {"free_cores", "u16"}, {"jobs", "u16"},
             {"total_count", "u16"}, {"totals", "array", "total_count", TOTAL} },
  [0x02] = { {"instance", "u32"} },
  [0x10] = { {"launcher_node", "u16"}, {"flags", "u16"}, {"launcher_addr", "ipv4"},
             {"launcher_port", "u16"}, {"threads_per_node", "u16"}, {"region_bytes", "u64"},
             {"binary_hash", "hash"}, {"nodes", "u16"}, {"", "rsv", 2}, {"path", "str16"},
             {"cwd", "str16"}, {"argc", "u16"}, {"argv", "str16s", "argc"} },
  [0x11] = { {"node", "u16"}, {"data_port", "u16"}, {"cores", "u16"}, {"", "rsv", 2},
             {"ram_commit", "u64"}, {"spill_commit", "u64"} },
  [0x12] = { {"count", "u16"}, {"", "rsv", 2},
             {"entries", "array", "count", { {"job_id", "u32"}, {"", "rsv", 4},
               {"chunks_for_peer", "u64"}, {"chunks_by_peer", "u64"}, {"segsec_for_peer", "u64"},
               {"segsec_by_peer", "u64"} }} },
  [0x13] = { {"node", "u16"}, {"reason", "u8"}, {"", "rsv", 1} },
  [0x14] = STATUS_TEXT,
  [0x20] = { {"role", "u8"}, {"", "rsv", 1}, {"listen_port", "u16"}, {"pid", "u32"},
             {"binary_hash", "hash"} },
  [0x21] = { {"segment_count", "u16"}, {"member_count", "u8"}, {"", "rsv", 1},
             {"members", "array", "member_count", MEMBER}, {"homes", "bytes", "segment_count"} },
  [0x22] = {},
  [0x23] = END,
  [0x24] = { {"epoch", "u32"} },
  [0x30] = PAGE_ID, [0x31] = PAGE_ID, [0x32] = PAGE_ID, [0x38] = PAGE, [0x39] = PAGE,
  [0x3A] = PAGE_ID,
  [0x3B] = { {"page_id", "pageid"}, {"epoch", "u32"}, {"home", "u16"}, {"", "rsv", 2} },
  [0x3C] = PAGE_ID, [0x40] = PAGE_ID, [0x41] = PAGE_ID, [0x42] = PAGE_ID, [0x43] = PAGE_ID,
  [0x44] = PAGE, [0x45] = PAGE, [0x46] = PAGE_ID,
  [0x50] = { {"thread", "u16"}, {"", "rsv", 2} },
  [0x51] = { {"chunk_id", "u64"}, {"task_id", "u64"}, {"lo", "u64"}, {"hi", "u64"},
             {"call_id", "u32"}, {"arg_len", "u16"}, {"", "rsv", 2}, {"arg", "bytes", "arg_len"} },
  [0x52] = { {"reason", "u8"}, {"", "rsv", 3} },
  [0x53] = { {"chunk_id", "u64"}, {"cpu_ns", "u64"}, {"thread", "u16"}, {"", "rsv", 6} },
  [0x60] = HANDLE_THREAD("lock_id"), [0x61] = HANDLE_STATUS("lock_id"),
  [0x62] = HANDLE_THREAD("lock_id"), [0x63] = HANDLE_THREAD("barrier_id"),
  [0x64] = HANDLE_STATUS("barrier_id"),
  [0x65] = { {"offset", "u64"}, {"operand", "u64"}, {"op", "u8"}, {"", "rsv", 7} },
  [0x66] = { {"offset", "u64"}, {"old", "u64"} },
  [0x70] = { {"segment_id", "u32"}, {"flags", "u16"}, {"entry_count", "u16"},
             {"entries", "migrate", "entry_count"} },
  [0x71] = { {"segment_id", "u32"} },
  [0x72] = { {"segments", "u32"} },
  [0x73] = { {"segment_id", "u32"}, {"node", "u16"}, {"", "rsv", 2} },
  [0x80] = { {"pid", "u32"}, {"role", "u8"}, {"", "rsv", 1}, {"data_port", "u16"},
             {"binary_hash", "hash"} },
  [0x81] = { {"region_bytes", "u64"}, {"threads_per_node", "u16"}, {"", "rsv", 6} },
  [0x82] = { {"quota", "u16"}, {"member_count", "u8"}, {"", "rsv", 1},
             {"members", "array", "member_count", MEMBER} },
  [0x83] = { {"needed", "u64"}, {"available", "u64"}, {"status", "status"}, {"message", "str16"} },
  [0x84] = { {"node", "u16"}, {"", "rsv", 2}, {"task_id", "u64"}, {"indexes", "u64"},
             {"cpu_ns", "u64"} },
  [0x85] = { {"epoch", "u32"}, {"count", "u8"}, {"", "rsv", 3},
             {"entries", "array", "count", { {"node", "u16"}, {"segments", "u16"} }} },
  [0x86] = END,
  [0x87] = { {"faults", "u64"}, {"pages_in", "u64"}, {"pages_out", "u64"}, {"retries", "u64"},
             {"latency", "u32s", 16}, {"segment_count", "u16"}, {"", "rsv", 2},
             {"segments", "array", "segment_count", { {"segment", "u32"}, {"home", "u16"},
               {"shared", "u16"}, {"modified", "u16"}, {"", "rsv", 2} }} },
  [0x90] = { {"threads", "u16"}, {"flags", "u16"}, {"ram_bytes", "u64"}, {"spill_bytes", "u64"} },
  [0x91] = { {"reason", "u8"}, {"", "rsv", 3} },
  [0x92] = STATUS_TEXT,
  [0x93] = { {"node", "u16"}, {"reason", "u8"}, {"", "rsv", 1} },
  [0x94] = MEMBER,
  [0xA0] = { {"nodes", "u16"}, {"", "rsv", 2}, {"binary_hash", "hash"}, {"path", "str16"},
             {"cwd", "str16"}, {"argc", "u16"}, {"argv", "str16s", "argc"} },
  [0xA1] = { {"job_id", "u32"}, {"node", "u16"}, {"", "rsv", 2} },
  [0xA2] = STATUS_TEXT,
  [0xA3] = { {"force", "u8"}, {"", "rsv", 3} },
  [0xA4] = STATUS_TEXT,
}

local SIZES = { u8 = 1, u16 = 2, u32 = 4, u64 = 8, ipv4 = 4, status = 2, hash = 32, pageid = 8 }

-- Walks `layout` over buf from `at`; returns the offset after it, or nil if buf is too short.
-- `values` collects the numbers read, by name, for counts further on.
local function walk(buf, at, layout, tree, flags, values)
  for _, field in ipairs(layout) do
    local name, kind, extra, sub = field[1], field[2], field[3], field[4]
    local fixed = SIZES[kind]
    if fixed then
      if buf:len() < at + fixed then return nil end
      local r = buf(at, fixed)
      if kind == "u64" then
        tree:add(pmsh, r, name .. ": " .. tostring(r:uint64()))
      elseif kind == "pageid" then
        tree:add(f.page, r)
      elseif kind == "ipv4" then
        tree:add(pmsh, r, name .. ": " .. tostring(r:ipv4()))
      elseif kind == "hash" then
        tree:add(pmsh, r, name .. ": " .. tostring(r:bytes()))
      else
        local v = r:uint()
        values[name] = v
        local label = name .. ": " .. v
        if kind == "status" then label = label .. " (" .. (STATUS[v] or "?") .. ")" end
        tree:add(pmsh, r, label)
      end
      at = at + fixed
    elseif kind == "rsv" then
      if buf:len() < at + extra then return nil end
      at = at + extra
    elseif kind == "str16" then
      if buf:len() < at + 2 then return nil end
      local n = buf(at, 2):uint()
      if buf:len() < at + 2 + n then return nil end
      tree:add(pmsh, buf(at, 2 + n), name .. ": \"" .. (n > 0 and buf(at + 2, n):string() or "") .. "\"")
      at = at + 2 + n
    elseif kind == "str16s" then
      for i = 1, values[extra] or 0 do
        at = walk(buf, at, { {name .. "[" .. i .. "]", "str16"} }, tree, flags, values)
        if not at then return nil end
      end
    elseif kind == "bytes" then
      local n = values[extra] or 0
      if buf:len() < at + n then return nil end
      if n > 0 then tree:add(pmsh, buf(at, n), name .. ": " .. n .. " bytes") end
      at = at + n
    elseif kind == "u32s" then
      if buf:len() < at + 4 * extra then return nil end
      local list = {}
      for i = 0, extra - 1 do list[#list + 1] = tostring(buf(at + 4 * i, 4):uint()) end
      tree:add(pmsh, buf(at, 4 * extra), name .. ": " .. table.concat(list, " "))
      at = at + 4 * extra
    elseif kind == "array" then
      for i = 1, values[extra] or 0 do
        local start = at
        local item = tree:add(pmsh, buf(at, 0), name .. " " .. i)
        at = walk(buf, at, sub, item, flags, {})
        if not at then return nil end
        item:set_len(at - start)
      end
    elseif kind == "page" then
      at = walk(buf, at, PAGE_ID, tree, flags, values)
      if not at then return nil end
      if bit.band(flags, 0x0002) ~= 0 then  -- COMPRESSED (X-page-encoding)
        at = walk(buf, at, { {"mode", "u8"}, {"", "rsv", 3}, {"page_crc", "u32"} }, tree, flags,
                  values)
        if not at or buf:len() < at + 64 then return nil end
        tree:add(pmsh, buf(at, 64), "word_map: 64 bytes")
        at = at + 64
        if buf:len() > at then
          tree:add(pmsh, buf(at, buf:len() - at), "words: " .. (buf:len() - at) .. " bytes")
          at = buf:len()
        end
      elseif bit.band(flags, 0x0001) == 0 then
        if buf:len() < at + 4096 then return nil end
        tree:add(pmsh, buf(at, 4096), "data: 4096 bytes")
        at = at + 4096
      end
    elseif kind == "migrate" then
      for i = 1, values[extra] or 0 do
        if buf:len() < at + 24 then return nil end
        local start = at
        local where = buf(at + 3, 1):uint()
        local item = tree:add(pmsh, buf(at, 0), "entry " .. i)
        at = walk(buf, at, { {"page", "u16"}, {"state", "u8"}, {"where", "u8"}, {"owner", "u16"},
                             {"", "rsv", 2}, {"version", "u32"}, {"", "rsv", 4},
                             {"copyset", "u64"} }, item, flags, {})
        if where == 1 then
          if buf:len() < at + 4096 then return nil end
          item:add(pmsh, buf(at, 4096), "data: 4096 bytes")
          at = at + 4096
        end
        item:set_len(at - start)
      end
    end
  end
  return at
end

-- One whole frame in buf.
local function dissect_frame(buf, pinfo, tree)
  local opcode = buf(5, 1):uint()
  local flags = buf(6, 2):uint()
  local length = buf(28, 4):uint()
  local name = OPCODES[opcode] or string.format("0x%02X", opcode)
  pinfo.cols.protocol = "PMSH"
  local info = name .. " job=" .. buf(8, 4):uint() .. " " .. buf(12, 2):uint() .. "→" ..
               buf(14, 2):uint()
  if buf(16, 8):uint64() ~= UInt64(0) then info = info .. " req=" .. tostring(buf(16, 8):uint64()) end
  local names = {}
  for bitv, flag in pairs(FLAGS) do
    if bit.band(flags, bitv) ~= 0 then names[#names + 1] = flag end
  end
  if #names > 0 then info = info .. " [" .. table.concat(names, ",") .. "]" end
  pinfo.cols.info:append((tostring(pinfo.cols.info) ~= "" and " | " or "") .. info)

  local root = tree:add(pmsh, buf(0, HEADER + length), "ParaMesh " .. name)
  local h = root:add(pmsh, buf(0, HEADER), "Header")
  h:add(f.magic, buf(0, 4)); h:add(f.version, buf(4, 1)); h:add(f.opcode, buf(5, 1))
  h:add(f.flags, buf(6, 2)):append_text(#names > 0 and (" (" .. table.concat(names, ", ") .. ")") or "")
  h:add(f.job, buf(8, 4)); h:add(f.src, buf(12, 2)); h:add(f.dst, buf(14, 2))
  h:add(f.req, buf(16, 8)); h:add(f.epoch, buf(24, 4)); h:add(f.len, buf(28, 4))
  h:add(f.crc, buf(32, 4))
  local layout = LAYOUT[opcode]
  local payload = buf(HEADER, length):tvb()
  local p = root:add(pmsh, buf(HEADER, length), "Payload (" .. length .. " bytes)")
  if not layout then
    p:add_proto_expert_info(malformed, "unknown opcode")
    return
  end
  local ended = walk(payload, 0, layout, p, flags, {})
  if ended == nil then
    p:add_proto_expert_info(malformed, "payload shorter than " .. name .. " needs")
  elseif ended ~= length then
    p:add_proto_expert_info(malformed, (length - ended) .. " bytes left over after " .. name)
  end
end

local function frame_length(buf, pinfo, offset)
  return HEADER + buf(offset + 28, 4):uint()
end

local function dissect_stream(buf, pinfo, tree)
  dissect_tcp_pdus(buf, tree, HEADER, frame_length, dissect_frame)
  return buf:len()
end

function pmsh.dissector(buf, pinfo, tree)
  if buf:len() < HEADER or buf(0, 4):uint() ~= MAGIC then return 0 end
  if pinfo.port_type == 2 then  -- TCP: frames may span or share segments
    return dissect_stream(buf, pinfo, tree)
  end
  dissect_frame(buf, pinfo, tree)
  return buf:len()
end

-- The data plane's ports are chosen at run time: a TCP stream that starts with a ParaMesh
-- header is ParaMesh from then on.
local function heuristic(buf, pinfo, tree)
  if buf:len() < 8 or buf(0, 4):uint() ~= MAGIC or buf(4, 1):uint() ~= 1 then return false end
  pinfo.conversation = pmsh
  dissect_stream(buf, pinfo, tree)
  return true
end

-- A datagram holds exactly one frame.
local function heuristic_udp(buf, pinfo, tree)
  if buf:len() < HEADER or buf(0, 4):uint() ~= MAGIC or buf(4, 1):uint() ~= 1 then return false end
  dissect_frame(buf, pinfo, tree)
  return true
end

DissectorTable.get("udp.port"):add(47000, pmsh)
DissectorTable.get("tcp.port"):add(47001, pmsh)
pmsh:register_heuristic("tcp", heuristic)
pmsh:register_heuristic("udp", heuristic_udp)
