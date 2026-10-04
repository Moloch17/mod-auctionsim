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

    -- between the borders and clear of the close button; one line, cut off if too long
    local title = f:CreateFontString(nil, "ARTWORK", "GameFontNormal")
    title:SetPoint("TOPLEFT", f, "TOPLEFT", 36, -16)
    title:SetPoint("TOPRIGHT", f, "TOPRIGHT", -36, -16)
    title:SetHeight(14)
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
local enabledCheckbox, startupScanCheckbox, marketModeCheckbox, replayBiddingCheckbox
local maxRequiredLevelBox, maxItemLevelBox, marketBotsBox, marketScaleBox
local playerSellHoursBox, playerLiquidityBox, playerQualityBonusCheckbox, playerGoldPerDayBox
local resultsLog                 -- ScrollingMessageFrame, created in BuildWindow
local pendingResultLines = {}    -- lines logged before the window exists
local setBotCharFrame, setBotCharInput

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

-- Market Scale is a small fraction (0.1 by default): keep three decimals.
local function FormatScaleValue(value)
    local n = tonumber(value) or 0
    if n < 0 then
        n = 0
    end
    local s = sformat("%.3f", n)
    return (s:gsub("0+$", ""):gsub("%.$", ""))
end

-- Text that may be long wraps inside its width (set by the caller, from anchors or
-- SetWidth) instead of running past its container; NonSpaceWrap breaks long tokens.
local function WrapText(fs)
    fs:SetJustifyH("LEFT")
    fs:SetJustifyV("TOP")
    fs:SetNonSpaceWrap(true)
    return fs
end

-- One-row label beside an edit box: fixed width and height, so text that doesn't fit
-- is cut off with "..." instead of running into the box.
local function CreateLabel(parent, text, x, y, width)
    local fs = parent:CreateFontString(nil, "ARTWORK", "GameFontNormalSmall")
    fs:SetPoint("TOPLEFT", x, y)
    fs:SetSize(width, ROW_HEIGHT)
    fs:SetJustifyH("LEFT")
    fs:SetJustifyV("MIDDLE")
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

-- One-line header. The caller anchors its left edge; the right edge stops at
-- `rightOf` (default: the parent), and its fixed height keeps it to one line.
local function CreateSectionHeader(parent, text, rightOf)
    local fs = parent:CreateFontString(nil, "ARTWORK", "GameFontNormal")
    fs:SetHeight(14)
    fs:SetJustifyH("LEFT")
    fs:SetText(text)
    fs.rightOf = rightOf or parent
    return fs
end

-- Second anchor for a header: its right edge, so the text has a width to stay in.
local function FitHeader(fs)
    fs:SetPoint("RIGHT", fs.rightOf, "RIGHT", -2, 0)
end

-- A checkbox template label in a fixed column: one line that stops at the column edge.
local function FitCheckLabel(textName, columnWidth)
    local text = _G[textName]
    text:SetWidth(columnWidth - 30)
    text:SetHeight(16)
    text:SetJustifyH("LEFT")
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
    -- the label stays inside the button: one line, cut off with "..." if too long
    local fs = btn:GetFontString()
    if fs then
        fs:SetWidth(width - 12)
        fs:SetHeight(ROW_HEIGHT - 6)
    end
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
    WrapText(prompt)
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

-- Help text renderer. Help.lua is a small Markdown subset (see its header): headings,
-- one level of nested bullets, **highlights** and `commands`. Each paragraph, heading
-- and list item becomes its own wrapped FontString, so headings stand out, bullets get
-- a hanging indent and the text flows to the window's width instead of keeping the
-- source's line breaks.
local HELP_STYLES = {
    h1 = {font = "GameFontNormalHuge", before = 0, after = 12},
    h2 = {font = "GameFontNormalLarge", before = 18, after = 8, rule = true},
    h3 = {font = "GameFontNormal", before = 12, after = 4},
    p = {font = "GameFontHighlight", before = 0, after = 9},
    li = {font = "GameFontHighlight", before = 0, after = 5},
}
local HELP_BULLET_INDENT, HELP_BULLET_GAP = 16, 14
local HELP_LINE_SPACING = 2

local function TrimHelpLine(line)
    return (line:match("^%s*(.-)%s*$"))
end

-- Blocks in reading order: {kind = "h1"|"h2"|"h3"|"p"|"li", level = 1|2, text = ...}.
local function ParseHelp(text)
    local blocks, current = {}, nil
    local function flush()
        if current then
            blocks[#blocks + 1] = current
            current = nil
        end
    end
    for line in (text .. "\n"):gmatch("(.-)\r?\n") do
        local hashes, heading = line:match("^(#+)%s+(.*)$")
        local indent, item = line:match("^(%s*)%-%s+(.*)$")
        if line:match("^%s*$") then
            flush()
        elseif hashes then
            flush()
            blocks[#blocks + 1] = {kind = "h" .. math.min(#hashes, 3), text = TrimHelpLine(heading)}
        elseif item then
            flush()
            current = {kind = "li", level = #indent >= 2 and 2 or 1, text = TrimHelpLine(item)}
        elseif current then
            current.text = current.text .. " " .. TrimHelpLine(line)
        else
            current = {kind = "p", text = TrimHelpLine(line)}
        end
    end
    flush()
    return blocks
end

-- **text** in gold, `text` in light blue.
local function HelpInline(text)
    text = text:gsub("%*%*(.-)%*%*", "|cffffd100%1|r")
    text = text:gsub("`(.-)`", "|cff9fd8ff%1|r")
    return text
end

-- Builds the blocks into `body` once; returns layout(width) -> content height.
local function BuildHelpBlocks(body, text)
    local items = {}
    for _, block in ipairs(ParseHelp(text)) do
        local style = HELP_STYLES[block.kind]
        local fs = WrapText(body:CreateFontString(nil, "ARTWORK", style.font))
        fs:SetSpacing(HELP_LINE_SPACING)
        fs:SetText(HelpInline(block.text))
        local item = {block = block, style = style, fs = fs}
        if block.kind == "li" then
            item.bullet = body:CreateFontString(nil, "ARTWORK", style.font)
            item.bullet:SetText(block.level == 1 and "\226\128\162" or "\226\128\147")  -- bullet / en dash
        end
        if style.rule then
            item.rule = body:CreateTexture(nil, "ARTWORK")
            item.rule:SetTexture(1, 0.82, 0, 0.25)
            item.rule:SetHeight(1)
        end
        items[#items + 1] = item
    end

    return function(width)
        local y, previous = 0, nil
        for i, item in ipairs(items) do
            local block, style = item.block, item.style
            if i > 1 then
                y = y + style.before
                if previous == "li" and block.kind ~= "li" then
                    y = y + 4  -- a little air after a list
                end
            end
            local x = 0
            if block.kind == "li" then
                x = (block.level - 1) * HELP_BULLET_INDENT
                item.bullet:ClearAllPoints()
                item.bullet:SetPoint("TOPLEFT", x, -y)
                x = x + HELP_BULLET_GAP
            end
            item.fs:ClearAllPoints()
            item.fs:SetPoint("TOPLEFT", x, -y)
            item.fs:SetWidth(width - x)
            y = y + item.fs:GetStringHeight()
            if item.rule then
                item.rule:ClearAllPoints()
                item.rule:SetPoint("TOPLEFT", 0, -(y + 3))
                item.rule:SetWidth(width)
                y = y + 4
            end
            y = y + style.after
            previous = block.kind
        end
        return y
    end
end

-- Scrollable, movable help windows (text from Help.lua): the main Help and the
-- EXPERIMENTAL FEATURES tab's Help.
local helpFrames = {}
local function BuildHelpPopup(name, titleText, bodyText)
    if helpFrames[name] then
        return helpFrames[name]
    end

    local f = CreateModuleWindow(name, 580, 520, TITLE_PREFIX .. titleText, "FULLSCREEN_DIALOG")

    local scroll = CreateFrame("ScrollFrame", name .. "Scroll", f, "UIPanelScrollFrameTemplate")
    scroll:SetPoint("TOPLEFT", 18, -38)
    scroll:SetPoint("BOTTOMRIGHT", -40, 46)

    local body = CreateFrame("Frame", name .. "Body", scroll)
    scroll:SetScrollChild(body)

    local layoutBlocks = BuildHelpBlocks(body, bodyText or "Help.lua is missing or failed to load.")
    local function layout()
        local w = scroll:GetWidth()
        if w <= 0 then
            return
        end
        body:SetWidth(w)
        body:SetHeight(math.max(layoutBlocks(w - 6) + 10, scroll:GetHeight()))
    end
    scroll:SetScript("OnSizeChanged", layout)
    f:HookScript("OnShow", layout)
    layout()

    local closeBtn = CreateCommandButton(f, "Close", 0, 0, 110, function() f:Hide() end)
    closeBtn:ClearAllPoints()
    closeBtn:SetPoint("BOTTOM", f, "BOTTOM", 0, 14)

    helpFrames[name] = f
    return f
end

local function ShowHelpPopup(name, titleText, bodyText)
    local f = BuildHelpPopup(name, titleText, bodyText)
    f:Show()
    f:Raise()
end

function AHSim.ShowHelp()
    ShowHelpPopup("AHSimHelpFrame", "Help", AHSim.helpText)
end

function AHSim.ShowExperimentalHelp()
    ShowHelpPopup("AHSimExperimentalHelpFrame", "Experimental Features Help", AHSim.experimentalHelpText)
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
    FitHeader(resultsHeader)

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

    -- Two pages above the shared Results log, switched by real tabs below the window
    -- (stock CharacterFrame-style tabs, managed by PanelTemplates).
    local mainPage = CreateFrame("Frame", "AHSimMainPage", panel)
    mainPage:SetPoint("TOPLEFT", panel, "TOPLEFT", 0, 0)
    mainPage:SetPoint("BOTTOMRIGHT", resultsBg, "TOPRIGHT", 0, 20)
    local experimentalPage = CreateFrame("Frame", "AHSimExperimentalPage", panel)
    experimentalPage:SetAllPoints(mainPage)
    experimentalPage:Hide()

    local pages = { mainPage, experimentalPage }
    local function SelectTab(index)
        PanelTemplates_SetTab(frame, index)
        for i, page in ipairs(pages) do
            if i == index then
                page:Show()
            else
                page:Hide()
            end
        end
    end
    -- PanelTemplates finds the tabs by name: <frame name>Tab<n>.
    local tabLabels = { "Main", "EXPERIMENTAL FEATURES" }
    for i, label in ipairs(tabLabels) do
        local tab = CreateFrame("Button", "AHSimFrameTab" .. i, frame, "CharacterFrameTabButtonTemplate")
        tab:SetID(i)
        tab:SetText(label)
        PanelTemplates_TabResize(tab, 0)  -- as wide as its label
        if i == 1 then
            tab:SetPoint("TOPLEFT", frame, "BOTTOMLEFT", 12, 7)
        else
            tab:SetPoint("LEFT", _G["AHSimFrameTab" .. (i - 1)], "RIGHT", -16, 0)
        end
        tab:SetScript("OnClick", function(self)
            SelectTab(self:GetID())
            PlaySound("igCharacterInfoTab")
        end)
    end
    PanelTemplates_SetNumTabs(frame, #tabLabels)
    SelectTab(1)

    -- Left column: checkboxes + action buttons.
    local actionsHeader = CreateSectionHeader(mainPage, "Actions")
    actionsHeader:SetWidth(LEFT_COLUMN_WIDTH - 2)
    actionsHeader:SetPoint("TOPLEFT", mainPage, "TOPLEFT", 2, 0)

    local leftColumn = CreateFrame("Frame", "AHSimLeftColumn", mainPage)
    leftColumn:SetPoint("TOPLEFT", mainPage, "TOPLEFT", 0, -HEADER_GAP)
    leftColumn:SetSize(LEFT_COLUMN_WIDTH, 1)

    local ly = 0

    enabledCheckbox = CreateFrame("CheckButton", "AHSimEnabledCheckbox", leftColumn, "UICheckButtonTemplate")
    enabledCheckbox:SetPoint("TOPLEFT", 0, -ly)
    FitCheckLabel("AHSimEnabledCheckboxText", LEFT_COLUMN_WIDTH)
    _G["AHSimEnabledCheckboxText"]:SetText("Enabled")
    enabledCheckbox:SetScript("OnClick", function(self)
        local on = self:GetChecked()
        SetConfigAndSave("Enabled", on and "1" or "0", on and "Enabled" or "Disabled")
    end)
    ly = ly + 26

    startupScanCheckbox = CreateFrame("CheckButton", "AHSimStartupScanCheckbox", leftColumn, "UICheckButtonTemplate")
    startupScanCheckbox:SetPoint("TOPLEFT", 0, -ly)
    FitCheckLabel("AHSimStartupScanCheckboxText", LEFT_COLUMN_WIDTH)
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
        leftColumn, "Run Queue", 0, -ly, LEFT_COLUMN_WIDTH, function() AHSim:Send(OP.RUNQUEUE) end)
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
    local settingsHeader = CreateSectionHeader(mainPage, "Listing Multipliers")
    settingsHeader:SetPoint("TOPLEFT", leftColumn, "TOPRIGHT", COLUMN_GAP + 2, HEADER_GAP)
    FitHeader(settingsHeader)

    local scrollFrame = CreateFrame("ScrollFrame", "AHSimScrollFrame", mainPage, "UIPanelScrollFrameTemplate")
    scrollFrame:SetPoint("TOPLEFT", leftColumn, "TOPRIGHT", COLUMN_GAP, 0)
    scrollFrame:SetPoint("BOTTOMRIGHT", mainPage, "BOTTOMRIGHT", -34, 0)

    local content = CreateFrame("Frame", "AHSimScrollContent", scrollFrame)
    scrollFrame:SetScrollChild(content)

    local y = 0

    CreateLabel(content, "Max Required Level:", 4, -y, 132)
    maxRequiredLevelBox = CreateNumberBox(content, 140, -y, 60, function(self)
        SetConfigAndSave("MaxRequiredLevel", self:GetText())
    end)

    CreateLabel(content, "Max Item Level:", 230, -y, 106)
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

    AHSim.BuildExperimentalPage(experimentalPage)
end

-- Replay Bidding only matters in Replay mode: while Market Mode is ticked the box is
-- greyed out and unclickable (its saved value is kept for a switch back), with a
-- note saying why. Called on load from the server's settings and on every toggle.
local REPLAY_BIDDING_NOTE = "Replay Bidding only applies when Market Mode is off."
local replayBiddingNote
-- The saved AuctionSim.Replay.Bidding (from the server, or the GM's last click). While
-- Market Mode is ticked the box is drawn unchecked, since nothing bids then; this keeps
-- the real value to show again when Market Mode is unticked.
local replayBiddingSaved = true
local function UpdateReplayBiddingState()
    if not replayBiddingCheckbox or not marketModeCheckbox then
        return
    end
    local text = _G["AHSimReplayBiddingCheckboxText"]
    if marketModeCheckbox:GetChecked() then
        replayBiddingCheckbox:SetChecked(false)
        replayBiddingCheckbox:Disable()
        text:SetTextColor(0.5, 0.5, 0.5)
        if replayBiddingNote then
            replayBiddingNote:SetText(REPLAY_BIDDING_NOTE)
        end
    else
        replayBiddingCheckbox:Enable()
        replayBiddingCheckbox:SetChecked(replayBiddingSaved)
        text:SetTextColor(NORMAL_FONT_COLOR.r, NORMAL_FONT_COLOR.g, NORMAL_FONT_COLOR.b)
        if replayBiddingNote then
            replayBiddingNote:SetText(" ")
        end
    end
end

-- EXPERIMENTAL FEATURES tab: Market mode, its settings and commands, and Replay
-- bidding. Everything here works but may still change between releases.
--
-- The page is a ScrollFrame of stacked rows. Each row measures itself for the
-- current width (wrapped text sets its height), so a long label pushes the rows
-- below it down instead of overlapping them, and the scroll bar takes over if the
-- whole tab ever outgrows the window.
function AHSim.BuildExperimentalPage(page)
    local scroll = CreateFrame("ScrollFrame", "AHSimExperimentalScroll", page, "UIPanelScrollFrameTemplate")
    scroll:SetPoint("TOPLEFT", page, "TOPLEFT", 0, 0)
    scroll:SetPoint("BOTTOMRIGHT", page, "BOTTOMRIGHT", -26, 0)

    local content = CreateFrame("Frame", "AHSimExperimentalContent", scroll)
    content:SetSize(1, 1)
    scroll:SetScrollChild(content)

    local ROW_GAP = 6
    local rows = {}  -- { frame = row, measure = function(width) -> height }

    local function AddRow(measure, gapAbove)
        local row = CreateFrame("Frame", nil, content)
        local previous = rows[#rows]
        if previous then
            row:SetPoint("TOPLEFT", previous.frame, "BOTTOMLEFT", 0, -(gapAbove or ROW_GAP))
        else
            row:SetPoint("TOPLEFT", content, "TOPLEFT", 0, 0)
        end
        row:SetPoint("RIGHT", content, "RIGHT", 0, 0)
        row:SetHeight(ROW_HEIGHT)
        rows[#rows + 1] = { frame = row, measure = measure, gap = gapAbove or ROW_GAP }
        return row
    end

    local function AddText(text, fontObject, gapAbove)
        local fs
        local row = AddRow(function(width)
            fs:SetWidth(width)
            return math.max(fs:GetStringHeight(), 12) + 2
        end, gapAbove)
        fs = WrapText(row:CreateFontString(nil, "ARTWORK", fontObject))
        fs:SetPoint("TOPLEFT", row, "TOPLEFT", 0, 0)
        fs:SetText(text)
        return fs
    end

    -- A checkbox whose template label wraps beside it and stops at the page edge.
    local function AddCheckbox(name, label, onClick)
        local cb
        local row = AddRow(function(width)
            local text = _G[name .. "Text"]
            text:SetWidth(math.max(width - 30, 40))
            return math.max(26, text:GetStringHeight() + 14)
        end)
        cb = CreateFrame("CheckButton", name, row, "UICheckButtonTemplate")
        cb:SetPoint("TOPLEFT", row, "TOPLEFT", 0, 0)
        local text = WrapText(_G[name .. "Text"])
        text:ClearAllPoints()
        text:SetPoint("TOPLEFT", cb, "TOPRIGHT", 0, -8)
        text:SetText(label)
        cb:SetScript("OnClick", onClick)
        return cb
    end

    -- A wrapped label on the left, an edit box beside it.
    local SETTING_LABEL_WIDTH = 110
    local function AddNumberSetting(label, onEnter, maxLetters, allowDecimal)
        local fs
        local row = AddRow(function()
            fs:SetWidth(SETTING_LABEL_WIDTH)
            return math.max(ROW_HEIGHT, fs:GetStringHeight() + 4)
        end)
        fs = WrapText(row:CreateFontString(nil, "ARTWORK", "GameFontNormalSmall"))
        fs:SetPoint("TOPLEFT", row, "TOPLEFT", 4, -4)
        fs:SetText(label)
        return CreateNumberBox(row, SETTING_LABEL_WIDTH + 10, 0, 60, onEnter, maxLetters, allowDecimal)
    end

    local COMMAND_WIDTH = 160
    local function AddCommand(label, onClick)
        local row = AddRow(function() return ROW_HEIGHT end)
        CreateCommandButton(row, label, 0, 0, COMMAND_WIDTH, onClick)
    end

    AddText("|cffff8000Experimental:|r these features work, but how they behave may still change between " ..
        "releases. Market Mode replaces the Replay bot with named seller bots and buyers learned from a real " ..
        "market (see Help, step 6). Replay Bidding turns the Replay bot's bidding on or off.",
        "GameFontHighlightSmall", 0)

    AddText("Settings", "GameFontNormal", 14)

    -- AuctionSim.Mode: unticked = Replay, ticked = Market. Takes effect at restart.
    marketModeCheckbox = AddCheckbox("AHSimMarketModeCheckbox", "Market Mode (restart to switch)", function(self)
        local on = self:GetChecked()
        SetConfigAndSave("Mode", on and "Market" or "Replay",
            (on and "Market" or "Replay") .. " mode saved -- restart the worldserver to switch.")
        UpdateReplayBiddingState()
        -- Saved either way; Yes restarts now, No names the mode still waiting for it.
        local popup = StaticPopup_Show("AHSIM_RESTART_FOR_MARKET")
        if popup then
            popup.data = on and "Market" or "Replay"
        end
    end)

    -- AuctionSim.Replay.Bidding: live from the next scan; unticking drops queued bids.
    -- Greyed out while Market Mode is ticked (UpdateReplayBiddingState).
    replayBiddingCheckbox = AddCheckbox(
        "AHSimReplayBiddingCheckbox", "Replay Bidding (the Replay bot bids and outbids)", function(self)
            local on = self:GetChecked() and true or false
            replayBiddingSaved = on
            SetConfigAndSave("ReplayBidding", on and "1" or "0",
                on and "Replay bidding on." or "Replay bidding off -- queued bids dropped.")
        end)
    replayBiddingCheckbox:SetScript("OnEnter", function(self)
        if not self:IsEnabled() then
            GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
            GameTooltip:SetText(REPLAY_BIDDING_NOTE, 1, 1, 1, 1, true)
            GameTooltip:Show()
        end
    end)
    replayBiddingCheckbox:SetScript("OnLeave", function() GameTooltip:Hide() end)
    replayBiddingNote = AddText(REPLAY_BIDDING_NOTE, "GameFontDisableSmall", 0)
    UpdateReplayBiddingState()

    -- AuctionSim.Market.*: Bots apply at restart / Market Reload; Scale from the next step.
    marketBotsBox = AddNumberSetting("Market Bots:", function(self)
        SetConfigAndSave("MarketBots", self:GetText(), "Market Bots saved -- applies at restart or Market Reload.")
    end, 4)
    marketScaleBox = AddNumberSetting("Market Scale:", function(self)
        local value = FormatScaleValue(self:GetText())
        self:SetText(value)
        SetConfigAndSave("MarketScale", value, "Market Scale saved.")
    end, 6, true)

    -- The player buyer (AuctionSim.Market.Player*): live from the next market step.
    AddText("Player buyer: buys players' listings at the market price (creates gold).",
        "GameFontHighlightSmall", 10)
    playerSellHoursBox = AddNumberSetting("Player Sell Hours:", function(self)
        local value = FormatScaleValue(self:GetText())
        self:SetText(value)
        SetConfigAndSave("PlayerSellHours", value, "Player Sell Hours saved.")
    end, 6, true)
    playerLiquidityBox = AddNumberSetting("Player Liquidity:", function(self)
        local value = FormatScaleValue(self:GetText())
        self:SetText(value)
        SetConfigAndSave("PlayerLiquidity", value, "Player Liquidity saved.")
    end, 6, true)
    playerQualityBonusCheckbox = AddCheckbox("AHSimPlayerQualityBonusCheckbox",
        "Quality Bonus (better items count as more in demand)", function(self)
            SetConfigAndSave("PlayerQualityBonus", self:GetChecked() and "1" or "0", "Quality Bonus saved.")
        end)
    playerGoldPerDayBox = AddNumberSetting("Player Gold / Day (0 = no limit):", function(self)
        SetConfigAndSave("PlayerGoldPerDay", self:GetText(), "Player Gold / Day saved.")
    end, 7)

    AddText("Market Commands", "GameFontNormal", 14)
    AddCommand("Market Status", function() AHSim:Send(OP.MARKETSTATUS) end)
    AddCommand("Market Fill", function() AHSim:Send(OP.MARKETFILL) end)
    AddCommand("Market Reload", function() AHSim:Send(OP.MARKETRELOAD) end)
    -- Dry run first: the server reports what it would delete and, if anything, asks
    -- (PURGEASK) for a second, confirming click.
    AddCommand("Market Purge", function() AHSim:Send(OP.MARKETPURGE) end)
    AddCommand("Help", function() AHSim.ShowExperimentalHelp() end)

    local function Relayout()
        local width = scroll:GetWidth()
        if not width or width <= 0 then
            return
        end
        content:SetWidth(width)
        local total = 0
        for i, row in ipairs(rows) do
            local height = row.measure(width)
            row.frame:SetHeight(height)
            total = total + height + (i > 1 and row.gap or 0)
        end
        content:SetHeight(math.max(total + 4, 1))
    end
    scroll:SetScript("OnSizeChanged", Relayout)
    page:SetScript("OnShow", Relayout)
    Relayout()
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
    elseif key == "Mode" then
        if marketModeCheckbox then marketModeCheckbox:SetChecked(value == "Market") end
        UpdateReplayBiddingState()
    elseif key == "ReplayBidding" then
        replayBiddingSaved = value == "1"
        UpdateReplayBiddingState()
    elseif key == "PlayerSellHours" then
        if playerSellHoursBox then playerSellHoursBox:SetText(FormatScaleValue(value)) end
    elseif key == "PlayerLiquidity" then
        if playerLiquidityBox then playerLiquidityBox:SetText(FormatScaleValue(value)) end
    elseif key == "PlayerQualityBonus" then
        if playerQualityBonusCheckbox then playerQualityBonusCheckbox:SetChecked(value == "1") end
    elseif key == "PlayerGoldPerDay" then
        if playerGoldPerDayBox then playerGoldPerDayBox:SetText(value) end
    elseif key == "MarketBots" then
        if marketBotsBox then marketBotsBox:SetText(value) end
    elseif key == "MarketScale" then
        if marketScaleBox then marketScaleBox:SetText(FormatScaleValue(value)) end
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

AHSim:RegisterHandler(OP.RUNQUEUERESULT, function(elapsedMs, count)
    AddResultLine(sformat("Ran %s queued action(s) in %sms.", count, elapsedMs))
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

-- NOTICE\t<kind>\t<a>\t<b>\t<moduleVersion> -- outdated config/data or a module<->
-- addon version mismatch. Shown once per distinct (kind, a, b, moduleVersion) via
-- AHSimDB, so a fixed problem stops nagging but a new release re-shows it.
local function ParseVer(s)
    local a, b, c = tostring(s):match("^(%d+)%.(%d+)%.?(%d*)")
    return tonumber(a) or 0, tonumber(b) or 0, tonumber(c) or 0
end
local function VerLess(x, y)
    local x1, x2, x3 = ParseVer(x)
    local y1, y2, y3 = ParseVer(y)
    if x1 ~= y1 then return x1 < y1 end
    if x2 ~= y2 then return x2 < y2 end
    return x3 < y3
end

AHSim:RegisterHandler(OP.NOTICE, function(kind, a, b, modVer)
    AHSimDB = AHSimDB or {}
    AHSimDB.seenNotices = AHSimDB.seenNotices or {}
    local key = table.concat({ tostring(kind), tostring(a), tostring(b), tostring(modVer) }, ":")
    if AHSimDB.seenNotices[key] then
        return
    end

    local text
    if kind == "config" then
        text = sformat(
            "|cffff8000AuctionSim:|r your auctionsim.conf is out of date (config schema v%s < v%s). " ..
            "A current auctionsim.conf.dist is in etc/modules/ -- merge the new keys.", a, b)
    elseif kind == "data" then
        text = sformat(
            "|cffff0000AuctionSim:|r auctionsim.dat is out of date (data schema v%s vs v%s). Pull the " ..
            "latest changes and rebuild the module -- the current data file ships with the repo and is " ..
            "redeployed on build.", a, b)
    elseif kind == "market" then
        text = sformat(
            "|cffff0000AuctionSim:|r Market mode is on but auctionsim_market.dat can't be used (file schema " ..
            "v%s, needs v%s; v0 = missing). The module is not running: put a current auctionsim_market.dat in " ..
            "etc/modules/ (rebuild the module to redeploy it) or untick Market Mode and restart. " ..
            "\".auctionsim market status\" shows the reason.", a, b)
    elseif kind == "version" then
        if a == "?" then
            text = sformat(
                "|cffff8000AuctionSim:|r the server module is v%s; your addon did not report a version " ..
                "-- reinstall the AHSim addon.", b)
        elseif VerLess(a, b) then
            text = sformat(
                "|cffff8000AuctionSim:|r your AHSim addon (v%s) is older than the server module (v%s) " ..
                "-- update the addon.", a, b)
        elseif VerLess(b, a) then
            text = sformat(
                "|cffff8000AuctionSim:|r the server module (v%s) is older than your AHSim addon (v%s) " ..
                "-- update / rebuild the module.", b, a)
        else
            AHSimDB.seenNotices[key] = true  -- cosmetic-only difference; don't nag
            return
        end
    else
        return
    end

    AHSimDB.seenNotices[key] = true
    AddResultLine(text)
end)

AHSim:RegisterHandler(OP.MARKETMSG, function(line)
    AddResultLine(line or "")
end)

StaticPopupDialogs["AHSIM_CONFIRM_PURGE"] = {
    -- StaticPopup_Show takes two text arguments in 3.3.5.
    text = "Delete the AuctionSim market sellers?\n\n%s and %s auction(s) will be deleted. Bidders get " ..
        "their gold back by mail. This can't be undone.",
    button1 = "Purge",
    button2 = "Cancel",
    OnAccept = function()
        AHSim:Send(OP.MARKETPURGE, "confirm")
    end,
    timeout = 0,
    whileDead = true,
    hideOnEscape = true,
    showAlert = true,
}

AHSim:RegisterHandler(OP.PURGEASK, function(accounts, characters, auctions)
    StaticPopup_Show("AHSIM_CONFIRM_PURGE",
        sformat("%s account(s), %s character(s)", accounts or "?", characters or "?"), auctions or "?")
end)

-- Shown whenever Market Mode is ticked or unticked (the change is already saved). Yes
-- asks the server for the stock 10 s restart; No leaves it for the GM's next restart.
-- self.data is the mode just chosen ("Market" or "Replay").
StaticPopupDialogs["AHSIM_RESTART_FOR_MARKET"] = {
    text = "Restart the worldserver now to apply the change?",
    button1 = "Yes",
    button2 = "No",
    OnAccept = function()
        AHSim:Send(OP.RESTARTWORLD)
    end,
    OnCancel = function(self)  -- No (or Escape)
        DEFAULT_CHAT_FRAME:AddMessage(
            (self.data or "Market") .. " mode cannot be initiated until the worldserver is restarted.")
    end,
    timeout = 0,
    whileDead = true,
    hideOnEscape = true,
    showAlert = true,
}
