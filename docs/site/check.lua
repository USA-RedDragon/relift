package.path = "docs/site/?.lua;" .. package.path
local navlib = require("nav")

local outdir = arg[1]
local nav = navlib.load()
local bad = 0

local function report(msg)
  io.stderr:write("docs/site: " .. msg .. "\n")
  bad = bad + 1
end

local function walk(dir, out)
  for _, name in ipairs(pandoc.system.list_directory(dir)) do
    local p = dir .. "/" .. name
    if navlib.is_dir(p) then
      walk(p, out)
    elseif name:match("%.html$") then
      table.insert(out, p:sub(#outdir + 2))
    end
  end
  return out
end

for _, it in ipairs(nav.missing) do
  report("docs/nav.md lists " .. it.title .. " but its page does not exist")
end
for _, it in ipairs(nav.pages) do
  if not navlib.read_file(outdir .. "/" .. it.out) then
    report("page " .. it.out .. " (" .. it.src .. ") is not in the site")
  end
end
for _, p in ipairs(navlib.extra(nav)) do
  report(p .. " is not in docs/nav.md")
end

local ids = {}
local function ids_of(page)
  if not ids[page] then
    local set = {}
    for id in (navlib.read_file(outdir .. "/" .. page) or ""):gmatch('%sid="([^"]*)"') do
      set[id] = true
    end
    ids[page] = set
  end
  return ids[page]
end

local function unescape(s)
  return (s:gsub("&quot;", '"'):gsub("&lt;", "<"):gsub("&gt;", ">"):gsub("&amp;", "&"))
end

local built = walk(outdir, {})
for _, page in ipairs(built) do
  local html = navlib.read_file(outdir .. "/" .. page)
  html = html:gsub("<script.-</script>", "")
  for _, attr in ipairs({ "href", "src" }) do
    for raw in html:gmatch("%s" .. attr .. '="([^"]*)"') do
      local url = unescape(raw)
      if not url:match("^%a[%w+.-]*:") and not url:match("^/") then
        local file, frag = url:match("^([^#?]*)[^#]*#?(.*)$")
        local target = page
        if file ~= "" then
          target = navlib.resolve(navlib.dir_of(page), file)
        end
        if not target or target == "" then
          report(page .. ": " .. url .. " leaves the site")
        elseif not navlib.read_file(outdir .. "/" .. target) then
          report(page .. ": " .. url .. " does not resolve to a file")
        elseif frag ~= "" and target:match("%.html$") and not ids_of(target)[frag] then
          report(page .. ": " .. url .. " names no id in " .. target)
        end
      end
    end
  end
end

if bad > 0 then
  io.stderr:write("docs/site: " .. bad .. " problem(s)\n")
  os.exit(1)
end
io.stderr:write("docs/site: " .. #built .. " pages, every link and image resolves\n")
