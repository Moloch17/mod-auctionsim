-- Bot Manager window contents. AHSim.BuildWindow() runs once from AHSimWindow.lua
-- once the server has confirmed this character is a GM.

local pairs = pairs
local ipairs = ipairs
local sformat = string.format
local smatch = string.match
local tinsert = table.insert
local OP = AHSim.OP

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

-- Tooltip-style outline shared by every free-form edit box.
local EDITBOX_BACKDROP = {
    bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true, tileSize = 16, edgeSize = 12,
    insets = { left = 3, right = 3, top = 3, bottom = 3 },
}

-- "AuctionSim  --  " with a real em dash; shared by every window title.
local TITLE_PREFIX = "AuctionSim  \226\128\148  "

local function StyleWindow(f)
    f:SetBackdrop(WINDOW_BACKDROP)
    f:SetBackdropColor(0, 0, 0, 1)
    f:SetBackdropBorderColor(1, 1, 1, 1)
end

-- Standalone draggable dialog: chrome, drag, Escape-to-close, title, close button.
-- Pass height = nil to set only the width (callers that size themselves later).
local function CreateModuleWindow(name, width, height, titleText, strata)
    local f = CreateFrame("Frame", name, UIParent)
    if height then
        f:SetSize(width, height)
    else
        f:SetWidth(width)
    end
    f:SetPoint("CENTER")
    f:SetFrameStrata(strata or "DIALOG")
    f:SetToplevel(true)
    StyleWindow(f)
    f:EnableMouse(true)
    f:SetMovable(true)
    f:RegisterForDrag("LeftButton")
    f:SetScript("OnDragStart", f.StartMoving)
    f:SetScript("OnDragStop", f.StopMovingOrSizing)
    f:SetClampedToScreen(true)
    f:Hide()
    tinsert(UISpecialFrames, name)

    local title = f:CreateFontString(nil, "ARTWORK", "GameFontNormal")
    title:SetPoint("TOP", f, "TOP", 0, -16)
    title:SetText(titleText)

    local close = CreateFrame("Button", nil, f, "UIPanelCloseButton")
    close:SetPoint("TOPRIGHT", f, "TOPRIGHT", -5, -5)

    return f
end

local CONTENT_INSET = 10
local TITLE_BAR_HEIGHT = 40

local LEFT_COLUMN_WIDTH = 130
local COLUMN_GAP = 20
local HEADER_GAP = 18

local RESULTS_HEIGHT = 130  -- viewport height; MAX_RESULT_LINES caps scrollback
local MAX_RESULT_LINES = 200

local maskEditBoxes = {}
local enabledCheckbox, startupScanCheckbox
local maxRequiredLevelBox, maxItemLevelBox
local resultsLog                 -- ScrollingMessageFrame, created in BuildWindow
local pendingResultLines = {}    -- lines logged before the window exists
local setBotCharFrame, setBotCharInput
local helpFrame

-- A ScrollingMessageFrame keeps its own line buffer and renders each line on its
-- own, so MAX_RESULT_LINES of scrollback works where a single giant FontString
-- would truncate. Newest line lands at the bottom (chat-style); scroll up for
-- history, wheel to move, shift-wheel to jump to an end.
local function AddResultLine(text)
    if resultsLog then
        resultsLog:AddMessage(text)
    else
        pendingResultLines[#pendingResultLines + 1] = text
    end
end

-- Apply + persist a setting right away, unlike the staged mask grid below. `note`,
-- if given, replaces the generic "Saved" line the resulting CONFIGSAVED prints.
-- Note: two quick edits before the first CONFIGSAVED lands will show the second
-- note for both replies -- acceptable for a manual GM panel.
local pendingSaveNote
local function SetConfigAndSave(key, value, note)
    pendingSaveNote = note
    AHSim:Send(OP.SETCONFIG, key, value)
    AHSim:Send(OP.SAVECONFIG)
end

local function FriendlyName(classKey)
    return string.gsub(classKey, "Percent$", "")
end

-- Mask cells are plain multipliers, same as auctionsim.conf (1, 1.5, 0.5, 0).
-- WoW 3.3.5's string.format has %f but not %g, so trim a fixed-precision result by
-- hand: 1.00 -> "1", 1.50 -> "1.5", 0.25 -> "0.25", anything < 0 -> "0".
local function FormatMaskValue(value)
    local n = tonumber(value) or 0
    if n < 0 then
        n = 0
    end
    local s = sformat("%.2f", n)
    return (s:gsub("0+$", ""):gsub("%.$", ""))
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

local function CreateNumberBox(parent, x, y, width, onEnter, maxLetters, allowDecimal)
    local box = CreateFrame("EditBox", nil, parent)
    box:SetSize(width, ROW_HEIGHT)
    box:SetPoint("TOPLEFT", x, y)
    box:SetAutoFocus(false)
    -- SetNumeric blocks the decimal point, so multiplier cells stay free-form and
    -- lean on FormatMaskValue (tonumber) to sanitize on commit instead.
    if not allowDecimal then
        box:SetNumeric(true)
    end
    box:SetMaxLetters(maxLetters or 3)
    box:SetFontObject("GameFontHighlightSmall")
    box:SetTextInsets(4, 4, 0, 0)

    -- outline so it reads as an editable field
    box:SetBackdrop(EDITBOX_BACKDROP)
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
    btn:SetSize(width, ROW_HEIGHT)
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
    AHSim:Send(OP.SETBOTCHAR, name)
end

local POPUP_WIDTH = 380
local POPUP_MARGIN = 20

local function BuildSetBotCharPopup()
    if setBotCharFrame then
        return
    end

    local f = CreateModuleWindow(
        "AHSimSetBotCharFrame", POPUP_WIDTH, nil, TITLE_PREFIX .. "Set Bot Character", "FULLSCREEN_DIALOG")

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
    setBotCharInput:SetBackdrop(EDITBOX_BACKDROP)
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

    local f = CreateModuleWindow("AHSimHelpFrame", 520, 480, TITLE_PREFIX .. "Help", "FULLSCREEN_DIALOG")

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
    local frame = CreateModuleWindow(
        "AHSimFrame", WINDOW_WIDTH, WINDOW_HEIGHT, TITLE_PREFIX .. "Bot Manager", "DIALOG")

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

    resultsLog = CreateFrame("ScrollingMessageFrame", "AHSimResultsLog", resultsBg)
    resultsLog:SetPoint("TOPLEFT", resultsBg, "TOPLEFT", 8, -8)
    resultsLog:SetPoint("BOTTOMRIGHT", resultsBg, "BOTTOMRIGHT", -10, 8)
    resultsLog:SetFontObject("GameFontHighlightSmall")
    resultsLog:SetJustifyH("LEFT")
    resultsLog:SetFading(false)
    resultsLog:SetMaxLines(MAX_RESULT_LINES)
    resultsLog:EnableMouseWheel(true)
    resultsLog:SetScript("OnMouseWheel", function(self, delta)
        if delta > 0 then
            if IsShiftKeyDown() then self:ScrollToTop() else self:ScrollUp() end
        else
            if IsShiftKeyDown() then self:ScrollToBottom() else self:ScrollDown() end
        end
    end)

    for _, line in ipairs(pendingResultLines) do
        resultsLog:AddMessage(line)
    end
    pendingResultLines = {}

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
    local buttonStep = ROW_HEIGHT + buttonGap
    CreateCommandButton(leftColumn, "Scan", 0, -ly, LEFT_COLUMN_WIDTH, function() AHSim:Send(OP.SCAN) end)
    ly = ly + buttonStep
    CreateCommandButton(leftColumn, "Delete", 0, -ly, LEFT_COLUMN_WIDTH, function() AHSim:Send(OP.DELETE) end)
    ly = ly + buttonStep
    CreateCommandButton(
        leftColumn, "Show Queue", 0, -ly, LEFT_COLUMN_WIDTH, function() AHSim:Send(OP.SHOWQUEUE) end)
    ly = ly + buttonStep
    CreateCommandButton(
        leftColumn, "Clean Over Cap", 0, -ly, LEFT_COLUMN_WIDTH, function() AHSim:Send(OP.CLEANOVERCAP) end)
    ly = ly + buttonStep
    CreateCommandButton(leftColumn, "Run Tests", 0, -ly, LEFT_COLUMN_WIDTH, function() AHSim:Send(OP.TEST) end)
    ly = ly + buttonStep
    CreateCommandButton(
        leftColumn, "Set Bot Char", 0, -ly, LEFT_COLUMN_WIDTH, function() AHSim.ShowSetBotCharPopup() end)
    ly = ly + buttonStep
    CreateCommandButton(leftColumn, "Help", 0, -ly, LEFT_COLUMN_WIDTH, function() AHSim.ShowHelp() end)
    ly = ly + ROW_HEIGHT

    leftColumn:SetHeight(ly)

    -- Right column: settings grid.
    local settingsHeader = CreateSectionHeader(panel, "Listing Multipliers")
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
    -- orderedMaskBoxes is the same boxes row-major (class outer, quality inner) so
    -- Tab can walk them.
    local orderedMaskBoxes = {}
    for _, classKey in ipairs(ITEM_CLASSES) do
        CreateCellLabel(content, FriendlyName(classKey), 0, -y, LABEL_COLUMN_WIDTH, ROW_HEIGHT)
        maskEditBoxes[classKey] = {}
        for i, quality in ipairs(QUALITIES) do
            local box = CreateNumberBox(
                content, LABEL_COLUMN_WIDTH + (i - 1) * COL_WIDTH, -y, COL_WIDTH - 4,
                function(self) self:SetText(FormatMaskValue(self:GetText())) end, 5, true)
            box:SetScript("OnEditFocusLost", function(self) self:SetText(FormatMaskValue(self:GetText())) end)
            maskEditBoxes[classKey][quality] = box
            orderedMaskBoxes[#orderedMaskBoxes + 1] = box
        end
        y = y + ROW_HEIGHT
    end

    -- Tab moves to the next cell in the row; from the last column it wraps to the
    -- first column of the next row. Shift-Tab goes the other way. The grid as a
    -- whole wraps at both ends. HighlightText so the landed cell is type-ready.
    local function FocusMaskCell(index)
        local n = #orderedMaskBoxes
        if n == 0 then
            return
        end
        index = (index - 1) % n + 1
        orderedMaskBoxes[index]:SetFocus()
        orderedMaskBoxes[index]:HighlightText()
    end

    for idx, box in ipairs(orderedMaskBoxes) do
        box:SetScript("OnTabPressed", function(self)
            self:SetText(FormatMaskValue(self:GetText()))  -- commit before moving
            FocusMaskCell(idx + (IsShiftKeyDown() and -1 or 1))
        end)
    end

    y = y + 8
    local settingsButtonWidth = 110
    local settingsButtonGap = 10
    CreateCommandButton(content, "Apply", 4, -y, settingsButtonWidth, function()
        -- Only send cells that differ from what the server last pushed (box.serverValue),
        -- so a normal edit is a handful of messages, not the full 119-cell grid.
        local count = 0
        for classKey, qualities in pairs(maskEditBoxes) do
            for quality, box in pairs(qualities) do
                local text = box:GetText()
                if text and text ~= "" then
                    local normalized = FormatMaskValue(text)
                    if normalized ~= box.serverValue then
                        AHSim:Send(OP.SETCONFIG, classKey .. "." .. quality, normalized)
                        box.serverValue = normalized  -- optimistic; server does not echo
                        count = count + 1
                    end
                end
            end
        end
        if count == 0 then
            AddResultLine("No changed multipliers to apply.")
        else
            AddResultLine(sformat(
                "|cff00ff00Applied %d changed value(s) in memory.|r Use Save To File to persist.", count))
        end
    end)

    CreateCommandButton(
        content, "Save To File", 4 + settingsButtonWidth + settingsButtonGap, -y, settingsButtonWidth,
        function() AHSim:Send(OP.SAVECONFIG) end)

    CreateCommandButton(
        content, "Refresh", 4 + (settingsButtonWidth + settingsButtonGap) * 2, -y, settingsButtonWidth,
        function() AHSim.RequestConfig(true) end)

    y = y + 30
    content:SetSize(LABEL_COLUMN_WIDTH + #QUALITIES * COL_WIDTH + 20, y)
end

AHSim:RegisterHandler(OP.CONFIG, function(key, value)
    if key == "Enabled" then
        if enabledCheckbox then enabledCheckbox:SetChecked(value == "1") end
    elseif key == "StartupScan" then
        if startupScanCheckbox then startupScanCheckbox:SetChecked(value == "1") end
    elseif key == "MaxRequiredLevel" then
        if maxRequiredLevelBox then maxRequiredLevelBox:SetText(value) end
    elseif key == "MaxItemLevel" then
        if maxItemLevelBox then maxItemLevelBox:SetText(value) end
    else
        local classKey, quality = smatch(key, "^(.-)%.(.+)$")
        if classKey and maskEditBoxes[classKey] and maskEditBoxes[classKey][quality] then
            local box = maskEditBoxes[classKey][quality]
            local normalized = FormatMaskValue(value)
            box:SetText(normalized)
            box.serverValue = normalized  -- baseline for the Apply diff
        end
    end
end)

AHSim:RegisterHandler(OP.CONFIGSAVED, function(status, message)
    if status == "ok" then
        AddResultLine("|cff00ff00" .. (pendingSaveNote or "Saved") .. "|r")
    else
        AddResultLine("|cffff0000" .. (message or "save failed") .. "|r")
    end
    pendingSaveNote = nil
end)

AHSim:RegisterHandler(OP.SCANRESULT, function(elapsedMs, added, total)
    AddResultLine(sformat("Scan complete in %sms. Added %s to queue (%s total).", elapsedMs, added, total))
end)

AHSim:RegisterHandler(OP.DELETERESULT, function(elapsedMs)
    AddResultLine(sformat("Deleted all bot auctions in %sms.", elapsedMs))
end)

AHSim:RegisterHandler(OP.QUEUEINFO, function(size, nextBuyIn, lastBuyIn)
    AddResultLine(sformat("Queue: %s item(s). Next buy in %ss, last buy in %ss.", size, nextBuyIn, lastBuyIn))
end)

AHSim:RegisterHandler(OP.CLEANRESULT, function(removed, elapsedMs)
    AddResultLine(sformat("Removed %s over-cap auction(s) in %sms.", removed, elapsedMs))
end)

AHSim:RegisterHandler(OP.TESTRESULT, function(index, total, status, name, detail)
    local color = (status == "pass") and "|cff00ff00" or "|cffff0000"
    AddResultLine(sformat("%s[%s/%s %s]|r %s: %s", color, index, total, string.upper(status), name, detail))
end)

AHSim:RegisterHandler(OP.TESTDONE, function(passed, total)
    AddResultLine(sformat("Test suite: %s/%s passed.", passed, total))
end)

AHSim:RegisterHandler(OP.ERROR, function(message)
    AddResultLine("|cffff0000Error: " .. (message or "unknown error") .. "|r")
end)

AHSim:RegisterHandler(OP.SETBOTCHARRESULT, function(status, name, characterId, accountId, note)
    if status == "ok" then
        AddResultLine(sformat(
            "|cff00ff00Set Bot Char:|r bot set to \"%s\" (character id %s, account id %s).%s",
            name or "?", characterId or "?", accountId or "?", (note and note ~= "") and (" " .. note) or ""))
        if AHSimDB then
            AHSimDB.botCharName = name
        end
        if setBotCharFrame then
            setBotCharFrame:Hide()
        end
    else
        -- failure: the server puts the reason in the first field
        AddResultLine("|cffff0000Set Bot Char failed:|r " .. (name or "unknown error"))
    end
end)
