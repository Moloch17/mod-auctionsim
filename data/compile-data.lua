local lfs = require("lfs")

local RowTypes = {
    ItemLink = 1,
    ItemLevel = 2,
    ItemClass = 3,
    ItemSubClass = 4,
    CharEquipSlot = 5,
    StartingBid = 6,
    TimeLeft = 7,
    Unknown = 8,
    ItemName = 9,
    IconPath = 10,
    StackSize = 11,
    Quality = 12,
    CanUse = 13,
    LevelReq = 14,
    CurrentMinBid = 15,
    MinIncrement = 16,
    Buyout = 17,
    BidAmount = 18,
    HasHighBidder = 19,
    Owner = 20,
    SaleStatus = 21,
    ScanRowID = 22,
    ItemID = 23,
    SuffixID = 24,
    PropertySeed = 25,
    Unknown1 = 26,
    PropertyScale = 27,
}

-- Usage:
--   lua compile-data.lua                                   ingest ./scans/, write auctionsim.dat (default)
--   lua compile-data.lua --worker <index> <count> <out>     ingest this process's shard of ./scans/, write a partial accumulator file
--   lua compile-data.lua --merge <out> <partial...>         combine partial accumulator files, write the final auctionsim.dat
--
-- Aggregation is streaming: only sum/count/min/max per (faction, item, suffix) is kept in
-- memory, never the individual per-auction prices, so memory stays proportional to the number
-- of distinct items rather than the number of auction rows across the whole scan corpus.

local function FactionNum(faction)
    if faction == "Alliance" then
        return 2
    end
    if faction == "Horde" then
        return 6
    end
    return 0
end

local function GetAccumulator(Data, factionNum, itemId, suffixId)
    local byItem = Data[factionNum]
    if byItem == nil then
        byItem = {}
        Data[factionNum] = byItem
    end
    local bySuffix = byItem[itemId]
    if bySuffix == nil then
        bySuffix = {}
        byItem[itemId] = bySuffix
    end
    local acc = bySuffix[suffixId]
    if acc == nil then
        acc = { sum = 0, count = 0, min = math.huge, max = 0 }
        bySuffix[suffixId] = acc
    end
    return acc
end

local function Accumulate(Data, factionNum, itemId, suffixId, price)
    local acc = GetAccumulator(Data, factionNum, itemId, suffixId)
    acc.sum = acc.sum + price
    acc.count = acc.count + 1
    if price > acc.max then
        acc.max = price
    end
    if price < acc.min then
        acc.min = price
    end
end

local function ScanPath()
    return "./scans/"
end

-- Sorted so shard assignment (index in the list) is deterministic across worker processes.
local function ListScanFiles(path)
    local files = {}
    for file in lfs.dir(path) do
        if file ~= "." and file ~= ".." then
            table.insert(files, file)
        end
    end
    table.sort(files)
    return files
end

local function IngestFiles(Data, path, files)
    for _, file in ipairs(files) do
        dofile(path .. file)
        print("Loading " .. file)
        for _, factions in pairs(AucScanData.scans) do
            for faction, data in pairs(factions) do
                if data.ropes then
                    local factionNum = FactionNum(faction)
                    for _, ropeStr in ipairs(data.ropes) do
                        local rows = loadstring(ropeStr)()
                        for _, row in ipairs(rows) do
                            local price = row[RowTypes.Buyout] / row[RowTypes.StackSize]
                            Accumulate(Data, factionNum, row[RowTypes.ItemID], row[RowTypes.SuffixID], price)
                        end
                    end
                end
            end
        end
    end
end

-- Writes a temp file then renames over outpath, so a crash/kill mid-write never leaves a
-- partially-written file behind for a consumer (or the next merge step) to read.
local function AtomicWriteLines(outpath, headerCount, lines)
    local tmpPath = outpath .. ".tmp"
    local file = assert(io.open(tmpPath, "w"))
    file:write(headerCount, "\n")
    for _, line in ipairs(lines) do
        file:write(line, "\n")
    end
    file:close()
    assert(os.rename(tmpPath, outpath))
end

-- Final output format is unchanged from the original script: header line is the row count,
-- then one "faction:item:suffix:mean:min:max" line per (faction, item, suffix).
-- A suffix whose retained prices sum to zero (e.g. every price was 0, meaning no usable buyout
-- data) is skipped, matching the original script's intent -- but only that one suffix, not the
-- rest of the item's suffixes like the original's buggy `break` did.
local function WriteFinal(Data, outpath)
    local lines = {}
    local total = 0
    for factionNum, items in pairs(Data) do
        for item, suffixes in pairs(items) do
            for suffix, acc in pairs(suffixes) do
                if acc.sum ~= 0 then
                    local mean = math.floor(acc.sum / acc.count)
                    table.insert(
                        lines,
                        ("%d:%d:%d:%d:%d:%d"):format(
                            factionNum, item, suffix, mean, math.floor(acc.min), math.floor(acc.max)))
                    total = total + 1
                end
            end
        end
    end
    AtomicWriteLines(outpath, total, lines)
end

-- Partial files carry the raw accumulator (not the final rounded mean/min/max) at full
-- precision, so merging shards produces exactly the same result as a single serial run.
local function WritePartial(Data, outpath)
    local lines = {}
    local total = 0
    for factionNum, items in pairs(Data) do
        for item, suffixes in pairs(items) do
            for suffix, acc in pairs(suffixes) do
                table.insert(
                    lines,
                    ("%d:%d:%d:%.17g:%d:%.17g:%.17g"):format(
                        factionNum, item, suffix, acc.sum, acc.count, acc.min, acc.max))
                total = total + 1
            end
        end
    end
    AtomicWriteLines(outpath, total, lines)
end

local function ReadPartial(Data, path)
    local file = assert(io.open(path, "r"))
    local total = tonumber(file:read("*l"))
    for _ = 1, total do
        local line = file:read("*l")
        local factionNum, item, suffix, sum, count, min, max =
            line:match("^(%-?%d+):(%-?%d+):(%-?%d+):([^:]+):(%-?%d+):([^:]+):([^:]+)$")
        local acc = GetAccumulator(Data, tonumber(factionNum), tonumber(item), tonumber(suffix))
        acc.sum = acc.sum + tonumber(sum)
        acc.count = acc.count + tonumber(count)
        min, max = tonumber(min), tonumber(max)
        if min < acc.min then
            acc.min = min
        end
        if max > acc.max then
            acc.max = max
        end
    end
    file:close()
end

local function RunSerial(outpath)
    local path = ScanPath()
    local files = ListScanFiles(path)
    local Data = {}
    IngestFiles(Data, path, files)
    print("All files loaded")
    print("Processing")
    WriteFinal(Data, outpath)
    print("Complete")
end

local function RunWorker(index, count, outpath)
    local path = ScanPath()
    local allFiles = ListScanFiles(path)
    local shardFiles = {}
    for i, file in ipairs(allFiles) do
        if (i - 1) % count == index then
            table.insert(shardFiles, file)
        end
    end
    local Data = {}
    IngestFiles(Data, path, shardFiles)
    WritePartial(Data, outpath)
end

local function RunMerge(outpath, partials)
    local Data = {}
    for _, p in ipairs(partials) do
        ReadPartial(Data, p)
    end
    WriteFinal(Data, outpath)
end

local mode = arg[1]
if mode == "--worker" then
    RunWorker(tonumber(arg[2]), tonumber(arg[3]), arg[4])
elseif mode == "--merge" then
    local partials = {}
    for i = 3, #arg do
        table.insert(partials, arg[i])
    end
    RunMerge(arg[2], partials)
else
    RunSerial("auctionsim.dat")
end
