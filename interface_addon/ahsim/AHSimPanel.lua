-- Bot Manager window contents. AHSim.BuildWindow() runs once from AHSimWindow.lua
-- once the server has confirmed this character is a GM.

local ITEM_CLASSES = {
    "ConsumablePercent", "ContainerPercent", "WeaponPercent", "GemPercent", "ArmorPercent",
    "ReagentPercent", "ProjectilePercent", "TradeGoodsPercent", "GenericPercent", "RecipePercent",
    "MoneyPercent", "QuiverPercent", "QuestPercent", "KeyPercent", "PermanentPercent",
    "MiscPercent", "GlyphPercent",
}

local QUALITIES = { "GREY", "WHITE", "GREEN", "BLUE", "PURPLE", "ORANGE", "YELLOW" }

local ROW_HEIGHT = 22
local COL_WIDTH = 60
local LABEL_COLUMN_WIDTH = 78  -- fits the longest class name centered

local WINDOW_WIDTH = 730
local WINDOW_HEIGHT = 700

-- Every module window: solid black fill, standard dialog border, full opacity.
local WINDOW_BACKDROP = {
    bgFile = "Interface\\Buttons\\WHITE8X8",
    edgeFile = "Interface\\DialogFrame\\UI-DialogBox-Border",
    tile = true, tileSize = 32, edgeSize = 32,
    insets = { left = 6, right = 6, top = 6, bottom = 6 },
}

local function StyleWindow(f)
    f:SetBackdrop(WINDOW_BACKDROP)
    f:SetBackdropColor(0, 0, 0, 1)
    f:SetBackdropBorderColor(1, 1, 1, 1)
end

local CONTENT_INSET = 10
local TITLE_BAR_HEIGHT = 40

local LEFT_COLUMN_WIDTH = 130
local COLUMN_GAP = 20
local HEADER_GAP = 18

local RESULTS_HEIGHT = 130  -- viewport only; MAX_RESULT_LINES caps history
local MAX_RESULT_LINES = 100

local maskEditBoxes = {}
local enabledCheckbox, startupScanCheckbox
local maxRequiredLevelBox, maxItemLevelBox
local resultsFontString, resultsScrollFrame, resultsContent
local resultLines = {}
local setBotCharFrame, setBotCharInput
local helpFrame

local function AddResultLine(text)
    table.insert(resultLines, 1, text)
    while #resultLines > MAX_RESULT_LINES do
        table.remove(resultLines)
    end
    if resultsFontString then
        resultsFontString:SetText(table.concat(resultLines, "\n"))
        -- newest line is index 1, so it's visible without scrolling
        resultsContent:SetHeight(math.max(RESULTS_HEIGHT, resultsFontString:GetStringHeight() + 8))
    end
end

-- Apply + persist a setting right away, unlike the staged mask grid below. `note`,
-- if given, replaces the generic "Saved" line the resulting CONFIGSAVED prints.
local pendingSaveNote
local function SetConfigAndSave(key, value, note)
    pendingSaveNote = note
    AHSim:Send("SETCONFIG", key, value)
    AHSim:Send("SAVECONFIG")
end

local function FriendlyName(classKey)
    return string.gsub(classKey, "Percent$", "")
end

local function CreateLabel(parent, text, x, y)
    local fs = parent:CreateFontString(nil, "ARTWORK", "GameFontNormalSmall")
    fs:SetPoint("TOPLEFT", x, y)
    fs:SetText(text)
    return fs
end

-- Label centered both ways within a w x h cell.
local function CreateCellLabel(parent, text, x, y, w, h)
    local fs = parent:CreateFontString(nil, "ARTWORK", "GameFontNormalSmall")
    fs:SetPoint("TOPLEFT", x, y)
    fs:SetSize(w, h)
    fs:SetJustifyH("CENTER")
    fs:SetJustifyV("MIDDLE")
    fs:SetText(text)
    return fs
end

local function CreateSectionHeader(parent, text)
    local fs = parent:CreateFontString(nil, "ARTWORK", "GameFontNormal")
    fs:SetText(text)
    return fs
end

local function CreateNumberBox(parent, x, y, width, onEnter)
    local box = CreateFrame("EditBox", nil, parent)
    box:SetSize(width, ROW_HEIGHT)
    box:SetPoint("TOPLEFT", x, y)
    box:SetAutoFocus(false)
    box:SetNumeric(true)
    box:SetMaxLetters(3)
    box:SetFontObject("GameFontHighlightSmall")
    box:SetTextInsets(4, 4, 0, 0)

    -- outline so it reads as an editable field
    box:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 12,
        insets = { left = 3, right = 3, top = 3, bottom = 3 },
    })
    box:SetBackdropColor(0, 0, 0, 0.45)
    box:SetBackdropBorderColor(0.65, 0.65, 0.65, 0.9)

    box:SetScript("OnEscapePressed", box.ClearFocus)
    box:SetScript("OnEnterPressed", function(self)
        onEnter(self)
        self:ClearFocus()
    end)
    return box
end

local function CreateCommandButton(parent, label, x, y, width, onClick)
    local btn = CreateFrame("Button", nil, parent, "UIPanelButtonTemplate")
    btn:SetSize(width, 22)
    btn:SetPoint("TOPLEFT", parent, "TOPLEFT", x, y)
    btn:SetText(label)
    btn:SetScript("OnClick", onClick)
    return btn
end

-- Set Bot Char dialog. Okay sends SETBOTCHAR; the server validates against the
-- characters DB and replies SETBOTCHARRESULT. A failure keeps this open, a success
-- closes it; both log to Results.
local function SubmitBotChar()
    local name = strtrim(setBotCharInput:GetText() or "")
    if name == "" then
        AddResultLine("|cffff0000Set Bot Char failed:|r no character name entered.")
        return
    end
    -- no self-character check: setup is often done while logged in as the bot
    AddResultLine("Set Bot Char: looking up \"" .. name .. "\" ...")
    AHSim:Send("SETBOTCHAR", name)
end

local POPUP_WIDTH = 380
local POPUP_MARGIN = 20

local function BuildSetBotCharPopup()
    if setBotCharFrame then
        return
    end

    local f = CreateFrame("Frame", "AHSimSetBotCharFrame", UIParent)
    f:SetWidth(POPUP_WIDTH)
    f:SetPoint("CENTER")
    f:SetFrameStrata("FULLSCREEN_DIALOG")
    f:SetToplevel(true)
    StyleWindow(f)
    f:EnableMouse(true)
    f:SetMovable(true)
    f:RegisterForDrag("LeftButton")
    f:SetScript("OnDragStart", f.StartMoving)
    f:SetScript("OnDragStop", f.StopMovingOrSizing)
    f:SetClampedToScreen(true)
    f:Hide()
    table.insert(UISpecialFrames, "AHSimSetBotCharFrame")

    local title = f:CreateFontString(nil, "ARTWORK", "GameFontNormal")
    title:SetPoint("TOP", f, "TOP", 0, -16)
    title:SetText("AuctionSim  \226\128\148  Set Bot Character")

    local close = CreateFrame("Button", nil, f, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", f, "TOPRIGHT", -5, -5)

    local promptTop = 40
    local prompt = f:CreateFontString(nil, "ARTWORK", "GameFontHighlight")
    prompt:SetWidth(POPUP_WIDTH - POPUP_MARGIN * 2)
    prompt:SetJustifyH("LEFT")
    prompt:SetJustifyV("TOP")
    prompt:SetPoint("TOPLEFT", POPUP_MARGIN, -promptTop)
    prompt:SetText(
        "Type in the character name that you want to use for the bot. Make sure the character is one " ..
        "that exists and is not one that you use to play the game.")

    setBotCharInput = CreateFrame("EditBox", "AHSimSetBotCharInput", f)
    setBotCharInput:SetSize(POPUP_WIDTH - POPUP_MARGIN * 2, 24)
    setBotCharInput:SetAutoFocus(false)
    setBotCharInput:SetMaxLetters(12)
    setBotCharInput:SetFontObject("GameFontHighlight")
    setBotCharInput:SetTextInsets(6, 6, 0, 0)
    setBotCharInput:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 12,
        insets = { left = 3, right = 3, top = 3, bottom = 3 },
    })
    setBotCharInput:SetBackdropColor(0, 0, 0, 0.45)
    setBotCharInput:SetBackdropBorderColor(0.65, 0.65, 0.65, 0.9)
    setBotCharInput:SetScript("OnEscapePressed", setBotCharInput.ClearFocus)
    setBotCharInput:SetScript("OnEnterPressed", SubmitBotChar)

    -- stack tight: title, wrapped prompt (measured), input, buttons
    local promptH = math.max(prompt:GetStringHeight(), 48)
    local inputTop = promptTop + promptH + 12
    setBotCharInput:SetPoint("TOPLEFT", POPUP_MARGIN, -inputTop)

    local btnW, btnGap = 100, 16
    local btnTop = inputTop + 24 + 14
    local btnX = (POPUP_WIDTH - (btnW * 2 + btnGap)) / 2
    CreateCommandButton(f, "Okay", btnX, -btnTop, btnW, SubmitBotChar)
    CreateCommandButton(f, "Close", btnX + btnW + btnGap, -btnTop, btnW, function() f:Hide() end)

    f:SetHeight(btnTop + 22 + POPUP_MARGIN)

    setBotCharFrame = f
end

function AHSim.ShowSetBotCharPopup()
    BuildSetBotCharPopup()
    setBotCharInput:SetText((AHSimDB and AHSimDB.botCharName) or "")
    setBotCharFrame:Show()
    setBotCharFrame:Raise()
    setBotCharInput:SetFocus()
end

-- Scrollable, movable window showing AHSim.helpText (from Help.lua).
local function BuildHelpPopup()
    if helpFrame then
        return
    end

    local f = CreateFrame("Frame", "AHSimHelpFrame", UIParent)
    f:SetSize(520, 480)
    f:SetPoint("CENTER")
    f:SetFrameStrata("FULLSCREEN_DIALOG")
    f:SetToplevel(true)
    StyleWindow(f)
    f:EnableMouse(true)
    f:SetMovable(true)
    f:RegisterForDrag("LeftButton")
    f:SetScript("OnDragStart", f.StartMoving)
    f:SetScript("OnDragStop", f.StopMovingOrSizing)
    f:SetClampedToScreen(true)
    f:Hide()
    table.insert(UISpecialFrames, "AHSimHelpFrame")

    local title = f:CreateFontString(nil, "ARTWORK", "GameFontNormal")
    title:SetPoint("TOP", f, "TOP", 0, -16)
    title:SetText("AuctionSim  \226\128\148  Help")

    local close = CreateFrame("Button", nil, f, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", f, "TOPRIGHT", -5, -5)

    local scroll = CreateFrame("ScrollFrame", "AHSimHelpScroll", f, "UIPanelScrollFrameTemplate")
    scroll:SetPoint("TOPLEFT", 12, -34)
    scroll:SetPoint("BOTTOMRIGHT", -38, 42)

    local body = CreateFrame("Frame", "AHSimHelpBody", scroll)
    scroll:SetScrollChild(body)

    local text = body:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
    text:SetPoint("TOPLEFT", 0, 0)
    text:SetJustifyH("LEFT")
    text:SetJustifyV("TOP")
    text:SetText(AHSim.helpText or "Help.lua is missing or failed to load.")

    local function layout()
        local w = scroll:GetWidth()
        if w <= 0 then
            return
        end
        body:SetWidth(w)
        text:SetWidth(w)
        body:SetHeight(math.max(text:GetStringHeight() + 8, scroll:GetHeight()))
    end
    scroll:SetScript("OnSizeChanged", layout)
    layout()

    local closeBtn = CreateCommandButton(f, "Close", 0, 0, 110, function() f:Hide() end)
    closeBtn:ClearAllPoints()
    closeBtn:SetPoint("BOTTOM", f, "BOTTOM", 0, 14)

    helpFrame = f
end

function AHSim.ShowHelp()
    BuildHelpPopup()
    helpFrame:Show()
    helpFrame:Raise()
end

function AHSim.BuildWindow()
    if AHSimFrame then
        return
    end

    AHSimDB = AHSimDB or {}

    -- standalone dialog, not an AH tab
    local frame = CreateFrame("Frame", "AHSimFrame", UIParent)
    frame:SetSize(WINDOW_WIDTH, WINDOW_HEIGHT)
    frame:SetPoint("CENTER")
    frame:SetFrameStrata("DIALOG")
    frame:SetToplevel(true)
    StyleWindow(frame)
    frame:EnableMouse(true)
    frame:SetMovable(true)
    frame:RegisterForDrag("LeftButton")
    frame:SetScript("OnDragStart", frame.StartMoving)
    frame:SetScript("OnDragStop", frame.StopMovingOrSizing)
    frame:SetClampedToScreen(true)
    frame:Hide()

    table.insert(UISpecialFrames, "AHSimFrame")  -- Escape closes it

    local title = frame:CreateFontString(nil, "ARTWORK", "GameFontNormal")
    title:SetPoint("TOP", frame, "TOP", 0, -16)
    title:SetText("AuctionSim  \226\128\148  Bot Manager")

    local close = CreateFrame("Button", nil, frame, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", frame, "TOPRIGHT", -5, -5)

    -- everything below anchors inside `panel`, so window-chrome offsets stop here
    local panel = CreateFrame("Frame", "AHSimPanel", frame)
    panel:SetPoint("TOPLEFT", frame, "TOPLEFT", CONTENT_INSET, -TITLE_BAR_HEIGHT)
    panel:SetPoint("BOTTOMRIGHT", frame, "BOTTOMRIGHT", -CONTENT_INSET, CONTENT_INSET)

    -- Results log: scrollable, full width along the bottom, shared by every action.
    local resultsBg = CreateFrame("Frame", nil, panel)
    resultsBg:SetPoint("BOTTOMLEFT", panel, "BOTTOMLEFT", 0, 0)
    resultsBg:SetPoint("BOTTOMRIGHT", panel, "BOTTOMRIGHT", 0, 0)
    resultsBg:SetHeight(RESULTS_HEIGHT)
    resultsBg:SetBackdrop({
        bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
        edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
        tile = true, tileSize = 16, edgeSize = 16,
        insets = { left = 4, right = 4, top = 4, bottom = 4 },
    })
    resultsBg:SetBackdropColor(0, 0, 0, 0.5)
    resultsBg:SetBackdropBorderColor(0.5, 0.5, 0.5, 1)

    local resultsHeader = CreateSectionHeader(panel, "Results")
    resultsHeader:SetPoint("BOTTOMLEFT", resultsBg, "TOPLEFT", 2, 4)

    resultsScrollFrame = CreateFrame("ScrollFrame", "AHSimResultsScrollFrame", resultsBg, "UIPanelScrollFrameTemplate")
    resultsScrollFrame:SetPoint("TOPLEFT", resultsBg, "TOPLEFT", 8, -8)
    resultsScrollFrame:SetPoint("BOTTOMRIGHT", resultsBg, "BOTTOMRIGHT", -34, 8)

    resultsContent = CreateFrame("Frame", "AHSimResultsContent", resultsScrollFrame)
    resultsContent:SetHeight(RESULTS_HEIGHT)
    resultsScrollFrame:SetScrollChild(resultsContent)
    -- scroll child needs an explicit width, and the scrollframe's isn't final yet here
    resultsScrollFrame:SetScript("OnSizeChanged", function(self)
        resultsContent:SetWidth(self:GetWidth())
    end)

    resultsFontString = resultsContent:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
    resultsFontString:SetPoint("TOPLEFT", resultsContent, "TOPLEFT", 0, 0)
    resultsFontString:SetPoint("TOPRIGHT", resultsContent, "TOPRIGHT", 0, 0)
    resultsFontString:SetJustifyH("LEFT")
    resultsFontString:SetJustifyV("TOP")
    resultsFontString:SetText("")

    -- Left column: checkboxes + action buttons.
    local actionsHeader = CreateSectionHeader(panel, "Actions")
    actionsHeader:SetPoint("TOPLEFT", panel, "TOPLEFT", 2, 0)

    local leftColumn = CreateFrame("Frame", "AHSimLeftColumn", panel)
    leftColumn:SetPoint("TOPLEFT", panel, "TOPLEFT", 0, -HEADER_GAP)
    leftColumn:SetSize(LEFT_COLUMN_WIDTH, 1)

    local ly = 0

    enabledCheckbox = CreateFrame("CheckButton", "AHSimEnabledCheckbox", leftColumn, "UICheckButtonTemplate")
    enabledCheckbox:SetPoint("TOPLEFT", 0, -ly)
    _G["AHSimEnabledCheckboxText"]:SetText("Enabled")
    enabledCheckbox:SetScript("OnClick", function(self)
        local on = self:GetChecked()
        SetConfigAndSave("Enabled", on and "1" or "0", on and "Enabled" or "Disabled")
    end)
    ly = ly + 26

    startupScanCheckbox = CreateFrame("CheckButton", "AHSimStartupScanCheckbox", leftColumn, "UICheckButtonTemplate")
    startupScanCheckbox:SetPoint("TOPLEFT", 0, -ly)
    _G["AHSimStartupScanCheckboxText"]:SetText("Startup Scan")
    startupScanCheckbox:SetScript("OnClick", function(self)
        SetConfigAndSave("StartupScan", self:GetChecked() and "1" or "0")
    end)
    ly = ly + 40

    local buttonGap = 6
    CreateCommandButton(leftColumn, "Scan", 0, -ly, LEFT_COLUMN_WIDTH, function() AHSim:Send("SCAN") end)
    ly = ly + 22 + buttonGap
    CreateCommandButton(leftColumn, "Delete", 0, -ly, LEFT_COLUMN_WIDTH, function() AHSim:Send("DELETE") end)
    ly = ly + 22 + buttonGap
    CreateCommandButton(
        leftColumn, "Show Queue", 0, -ly, LEFT_COLUMN_WIDTH, function() AHSim:Send("SHOWQUEUE") end)
    ly = ly + 22 + buttonGap
    CreateCommandButton(
        leftColumn, "Clean Over Cap", 0, -ly, LEFT_COLUMN_WIDTH, function() AHSim:Send("CLEANOVERCAP") end)
    ly = ly + 22 + buttonGap
    CreateCommandButton(leftColumn, "Run Tests", 0, -ly, LEFT_COLUMN_WIDTH, function() AHSim:Send("TEST") end)
    ly = ly + 22 + buttonGap
    CreateCommandButton(
        leftColumn, "Set Bot Char", 0, -ly, LEFT_COLUMN_WIDTH, function() AHSim.ShowSetBotCharPopup() end)
    ly = ly + 22 + buttonGap
    CreateCommandButton(leftColumn, "Help", 0, -ly, LEFT_COLUMN_WIDTH, function() AHSim.ShowHelp() end)
    ly = ly + 22

    leftColumn:SetHeight(ly)

    -- Right column: settings grid.
    local settingsHeader = CreateSectionHeader(panel, "Listing Settings")
    settingsHeader:SetPoint("TOPLEFT", leftColumn, "TOPRIGHT", COLUMN_GAP + 2, HEADER_GAP)

    local scrollFrame = CreateFrame("ScrollFrame", "AHSimScrollFrame", panel, "UIPanelScrollFrameTemplate")
    scrollFrame:SetPoint("TOPLEFT", leftColumn, "TOPRIGHT", COLUMN_GAP, 0)
    scrollFrame:SetPoint("BOTTOMRIGHT", resultsBg, "TOPRIGHT", -34, 20)

    local content = CreateFrame("Frame", "AHSimScrollContent", scrollFrame)
    scrollFrame:SetScrollChild(content)

    local y = 0

    CreateLabel(content, "Max Required Level:", 4, -y)
    maxRequiredLevelBox = CreateNumberBox(content, 140, -y, 60, function(self)
        SetConfigAndSave("MaxRequiredLevel", self:GetText())
    end)

    CreateLabel(content, "Max Item Level:", 230, -y)
    maxItemLevelBox = CreateNumberBox(content, 340, -y, 60, function(self)
        SetConfigAndSave("MaxItemLevel", self:GetText())
    end)

    y = y + 30

    -- Faint checkerboard behind the data rows so a cell tracks to its row and
    -- column. The quality-title row stays untinted. Drawn before the labels/boxes.
    local gridTop = y
    local gridWidth = LABEL_COLUMN_WIDTH + #QUALITIES * COL_WIDTH
    local dataTop = gridTop + ROW_HEIGHT
    local dataHeight = #ITEM_CLASSES * ROW_HEIGHT

    for i = 1, #QUALITIES do
        if i % 2 == 1 then
            local colTint = content:CreateTexture(nil, "BACKGROUND")
            colTint:SetPoint("TOPLEFT", LABEL_COLUMN_WIDTH + (i - 1) * COL_WIDTH - 2, -dataTop)
            colTint:SetSize(COL_WIDTH, dataHeight)
            colTint:SetTexture(1, 1, 1, 0.05)
        end
    end

    for r = 1, #ITEM_CLASSES do
        if r % 2 == 0 then
            local rowTint = content:CreateTexture(nil, "BACKGROUND")
            rowTint:SetPoint("TOPLEFT", 0, -(gridTop + r * ROW_HEIGHT))
            rowTint:SetSize(gridWidth, ROW_HEIGHT)
            rowTint:SetTexture(1, 1, 1, 0.07)
        end
    end

    for i, quality in ipairs(QUALITIES) do
        CreateCellLabel(content, quality, LABEL_COLUMN_WIDTH + (i - 1) * COL_WIDTH, -y, COL_WIDTH, ROW_HEIGHT)
    end
    y = y + ROW_HEIGHT

    -- 17 x 7 mask boxes; Apply sends them all at once rather than per-keystroke.
    for _, classKey in ipairs(ITEM_CLASSES) do
        CreateCellLabel(content, FriendlyName(classKey), 0, -y, LABEL_COLUMN_WIDTH, ROW_HEIGHT)
        maskEditBoxes[classKey] = {}
        for i, quality in ipairs(QUALITIES) do
            local box = CreateNumberBox(
                content, LABEL_COLUMN_WIDTH + (i - 1) * COL_WIDTH, -y, COL_WIDTH - 4, function() end)
            maskEditBoxes[classKey][quality] = box
        end
        y = y + ROW_HEIGHT
    end

    y = y + 8
    local settingsButtonWidth = 110
    local settingsButtonGap = 10
    CreateCommandButton(content, "Apply", 4, -y, settingsButtonWidth, function()
        local count = 0
        for classKey, qualities in pairs(maskEditBoxes) do
            for quality, box in pairs(qualities) do
                local text = box:GetText()
                if text and text ~= "" then
                    AHSim:Send("SETCONFIG", classKey .. "." .. quality, text)
                    count = count + 1
                end
            end
        end
        AddResultLine(string.format(
            "|cff00ff00Applied %d value(s) in memory.|r Use Save To File to persist.", count))
    end)

    CreateCommandButton(
        content, "Save To File", 4 + settingsButtonWidth + settingsButtonGap, -y, settingsButtonWidth,
        function() AHSim:Send("SAVECONFIG") end)

    CreateCommandButton(
        content, "Refresh", 4 + (settingsButtonWidth + settingsButtonGap) * 2, -y, settingsButtonWidth,
        function() AHSim:Send("GETCONFIG") end)

    y = y + 30
    content:SetSize(LABEL_COLUMN_WIDTH + #QUALITIES * COL_WIDTH + 20, y)
end

AHSim:RegisterHandler("CONFIG", function(key, value)
    if key == "Enabled" then
        if enabledCheckbox then enabledCheckbox:SetChecked(value == "1") end
    elseif key == "StartupScan" then
        if startupScanCheckbox then startupScanCheckbox:SetChecked(value == "1") end
    elseif key == "MaxRequiredLevel" then
        if maxRequiredLevelBox then maxRequiredLevelBox:SetText(value) end
    elseif key == "MaxItemLevel" then
        if maxItemLevelBox then maxItemLevelBox:SetText(value) end
    else
        local classKey, quality = string.match(key, "^(.-)%.(.+)$")
        if classKey and maskEditBoxes[classKey] and maskEditBoxes[classKey][quality] then
            maskEditBoxes[classKey][quality]:SetText(value)
        end
    end
end)

AHSim:RegisterHandler("CONFIGSAVED", function(status, message)
    if status == "ok" then
        AddResultLine("|cff00ff00" .. (pendingSaveNote or "Saved") .. "|r")
    else
        AddResultLine("|cffff0000" .. (message or "save failed") .. "|r")
    end
    pendingSaveNote = nil
end)

AHSim:RegisterHandler("SCANRESULT", function(elapsedMs, added, total)
    AddResultLine(string.format("Scan complete in %sms. Added %s to queue (%s total).", elapsedMs, added, total))
end)

AHSim:RegisterHandler("DELETERESULT", function(elapsedMs)
    AddResultLine(string.format("Deleted all bot auctions in %sms.", elapsedMs))
end)

AHSim:RegisterHandler("QUEUEINFO", function(size, nextBuyIn, lastBuyIn)
    AddResultLine(string.format("Queue: %s item(s). Next buy in %ss, last buy in %ss.", size, nextBuyIn, lastBuyIn))
end)

AHSim:RegisterHandler("CLEANRESULT", function(removed, elapsedMs)
    AddResultLine(string.format("Removed %s over-cap auction(s) in %sms.", removed, elapsedMs))
end)

AHSim:RegisterHandler("TESTRESULT", function(index, total, status, name, detail)
    local color = (status == "pass") and "|cff00ff00" or "|cffff0000"
    AddResultLine(string.format("%s[%s/%s %s]|r %s: %s", color, index, total, string.upper(status), name, detail))
end)

AHSim:RegisterHandler("TESTDONE", function(passed, total)
    AddResultLine(string.format("Test suite: %s/%s passed.", passed, total))
end)

AHSim:RegisterHandler("ERROR", function(message)
    AddResultLine("|cffff0000Error: " .. (message or "unknown error") .. "|r")
end)

-- ok: name, characterId, accountId, note   fail: reason
AHSim:RegisterHandler("SETBOTCHARRESULT", function(status, a, b, c, d)
    if status == "ok" then
        AddResultLine(string.format(
            "|cff00ff00Set Bot Char:|r bot set to \"%s\" (character id %s, account id %s).%s",
            a or "?", b or "?", c or "?", (d and d ~= "") and (" " .. d) or ""))
        if AHSimDB then
            AHSimDB.botCharName = a
        end
        if setBotCharFrame then
            setBotCharFrame:Hide()
        end
    else
        AddResultLine("|cffff0000Set Bot Char failed:|r " .. (a or "unknown error"))
    end
end)
