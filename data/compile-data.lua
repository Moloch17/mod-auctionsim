local lfs = require("lfs")
local path = "./scans/"

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

local total = 0

local Data = {}

local function EnsureTableExists(_table, _key)
    if _table[_key] == nil then
        _table[_key] = {}
    end
end

local function AddToData(_rows, _faction)
    for _, row in ipairs(_rows) do
        EnsureTableExists(Data, _faction)
        EnsureTableExists(Data[_faction], row[RowTypes.ItemID])
        EnsureTableExists(
            Data[_faction][row[RowTypes.ItemID]],
            row[RowTypes.SuffixID]
        )
        local buyout = {}
        EnsureTableExists(
            Data[_faction][row[RowTypes.ItemID]][row[RowTypes.SuffixID]],
            buyout
        )
        table.insert(
            Data[_faction][row[RowTypes.ItemID]][row[RowTypes.SuffixID]][buyout],
            row[RowTypes.Buyout] / row[RowTypes.StackSize]
        )
    end
end

for file in lfs.dir(path) do
    if file ~= "." and file ~= ".." then
        dofile(path .. file)
        print("Loading " .. file)
        for realm, factions in pairs(AucScanData.scans) do
            for faction, data in pairs(factions) do
                if data.ropes then
                    for _, ropeStr in ipairs(data.ropes) do
                        local rows = loadstring(ropeStr)()
                        AddToData(rows, faction)
                    end
                end
            end
        end
    end
end

print("All files loaded")
print("Processing")

local buffer = {}

for faction, items in pairs(Data) do
    for item, suffixes in pairs(items) do
        for suffix, buyouts in pairs(suffixes) do
            local sum = 0
            local count = 0
            local min = math.huge
            local max = 0
            for buyout, prices in pairs(buyouts) do
                for _, price in pairs(prices) do
                    sum = sum + price
                    count = count + 1
                    if price > max then
                        max = price
                    end
                    if price < min then
                        min = price
                    end
                end
            end
            if sum == 0 then
                break
            end
            local mean = math.floor(sum / count)
            local factionNum = 0
            if faction == "Alliance" then
                factionNum = 0
            end
            if faction == "Horde" then
                factionNum = 1
            end
            table.insert(
                buffer,
                ("%d:%d:%d:%d:%d:%d"):format(
                    factionNum,
                    item,
                    suffix,
                    mean,
                    math.floor(min),
                    math.floor(max)
                )
            )
            total = total + 1
        end
    end
end

local file = io.open("auctionsim.dat", "w")
file:write(total, "\n")

for _, value in ipairs(buffer) do
    file:write(value, "\n")
end

print("Complete")
