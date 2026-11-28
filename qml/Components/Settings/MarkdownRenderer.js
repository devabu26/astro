.pragma library

function escapeHtml(value) {
    return String(value)
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;")
        .replace(/'/g, "&#39;")
}

function escapeAttribute(value) {
    return escapeHtml(value)
}

function inlineMarkdown(value) {
    var tokens = []

    function protect(html) {
        var marker = "\u0001" + tokens.length + "\u0002"
        tokens.push(html)
        return marker
    }

    var text = String(value)

    // Protect links and images before escaping ordinary text.
    text = text.replace(/!\[([^\]]*)\]\(([^ )]+)(?:\s+"([^"]*)")?\)/g,
        function(match, label, url, title) {
            var image = '<img src="' + escapeAttribute(url)
                + '" alt="' + escapeAttribute(label) + '"'
            if (title)
                image += ' title="' + escapeAttribute(title) + '"'
            return protect(image + ' />')
        })

    text = text.replace(/\[([^\]]+)\]\(([^ )]+)(?:\s+"([^"]*)")?\)/g,
        function(match, label, url, title) {
            var link = '<a href="' + escapeAttribute(url) + '"'
            if (title)
                link += ' title="' + escapeAttribute(title) + '"'
            return protect(link + '>' + escapeHtml(label) + '</a>')
        })

    text = escapeHtml(text)

    text = text.replace(/`([^`]+)`/g,
        '<code style="color:#d6c6ff;">$1</code>')
    text = text.replace(/\*\*([^*]+)\*\*/g, '<b>$1</b>')
    text = text.replace(/__([^_]+)__/g, '<b>$1</b>')
    text = text.replace(/~~([^~]+)~~/g, '<s>$1</s>')
    text = text.replace(/\*([^*]+)\*/g, '<i>$1</i>')
    text = text.replace(/_([^_]+)_/g, '<i>$1</i>')

    text = text.replace(/\u0001(\d+)\u0002/g,
        function(match, index) {
            return tokens[Number(index)] || ""
        })

    return text
}

function splitTableRow(line) {
    var value = String(line).trim()
    if (value.charAt(0) === "|")
        value = value.substring(1)
    if (value.charAt(value.length - 1) === "|")
        value = value.substring(0, value.length - 1)

    return value.split("|")
}

function isTableSeparator(line) {
    return /^\s*\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)+\|?\s*$/.test(line)
}

function isBlockStart(lines, index) {
    var line = lines[index]
    if (!line || !line.trim())
        return true

    return /^\s{0,3}#{1,6}\s+/.test(line)
        || /^\s{0,3}(```|~~~)/.test(line)
        || /^\s{0,3}>/.test(line)
        || /^\s{0,3}([-*_])(?:\s*\1){2,}\s*$/.test(line)
        || /^\s{0,3}[-*+]\s+/.test(line)
        || /^\s{0,3}\d+[.)]\s+/.test(line)
        || /^\s*</.test(line)
        || (index + 1 < lines.length && line.indexOf("|") !== -1
            && isTableSeparator(lines[index + 1]))
}

function markdownToHtml(markdown) {
    var lines = String(markdown).replace(/\r\n?/g, "\n").split("\n")
    var output = []
    var i = 0

    while (i < lines.length) {
        var line = lines[i]

        if (!line.trim()) {
            i++
            continue
        }

        // Fenced code blocks.
        var fence = line.match(/^\s{0,3}(```|~~~)(.*)$/)
        if (fence) {
            var fenceMarker = fence[1]
            var codeLines = []
            i++
            while (i < lines.length
                    && !new RegExp("^\\s{0,3}" + fenceMarker + "+\\s*$").test(lines[i])) {
                codeLines.push(lines[i])
                i++
            }
            if (i < lines.length)
                i++

            output.push('<pre style="background-color:#17232d; color:#dce4eb; padding:8px;">'
                + '<code>' + escapeHtml(codeLines.join("\n")) + '</code></pre>')
            continue
        }

        // Raw HTML blocks in the project README (logo, badges and centered intro).
        if (/^\s*</.test(line)) {
            var htmlLines = []
            while (i < lines.length && lines[i].trim()) {
                htmlLines.push(lines[i])
                i++
            }
            var rawHtml = htmlLines.join("\n")
            rawHtml = rawHtml.replace(
                /https:\/\/github\.com\/devabu26\/astro\/blob\/main\//g,
                "https://raw.githubusercontent.com/devabu26/astro/main/"
            )
            output.push(rawHtml)
            continue
        }

        // ATX headings.
        var heading = line.match(/^\s{0,3}(#{1,6})\s+(.+?)\s*#*\s*$/)
        if (heading) {
            var level = heading[1].length
            var headingSize = [ "xx-large", "x-large", "large", "medium", "small", "small" ][level - 1]
            output.push('<h' + level + ' style="color:#f4f6f8; font-size:'
                + headingSize + ';">' + inlineMarkdown(heading[2]) + '</h' + level + '>')
            i++
            continue
        }

        // Horizontal rules.
        if (/^\s{0,3}([-*_])(?:\s*\1){2,}\s*$/.test(line)) {
            output.push('<hr />')
            i++
            continue
        }

        // GitHub-style tables.
        if (line.indexOf("|") !== -1 && i + 1 < lines.length
                && isTableSeparator(lines[i + 1])) {
            var headers = splitTableRow(line)
            i += 2
            var rows = []
            while (i < lines.length && lines[i].trim()
                    && lines[i].indexOf("|") !== -1) {
                rows.push(splitTableRow(lines[i]))
                i++
            }

            var table = '<table border="1" cellspacing="0" cellpadding="5" width="100%">'
                + '<thead><tr>'
            for (var h = 0; h < headers.length; h++) {
                table += '<th bgcolor="#263746"><b>'
                    + inlineMarkdown(headers[h].trim()) + '</b></th>'
            }
            table += '</tr></thead><tbody>'
            for (var r = 0; r < rows.length; r++) {
                table += '<tr>'
                for (var col = 0; col < headers.length; col++) {
                    var cell = col < rows[r].length ? rows[r][col] : ""
                    table += '<td>' + inlineMarkdown(cell.trim()) + '</td>'
                }
                table += '</tr>'
            }
            table += '</tbody></table>'
            output.push(table)
            continue
        }

        // Blockquotes.
        if (/^\s{0,3}>/.test(line)) {
            var quoteLines = []
            while (i < lines.length && /^\s{0,3}>/.test(lines[i])) {
                quoteLines.push(lines[i].replace(/^\s{0,3}>\s?/, ""))
                i++
            }
            output.push('<blockquote style="color:#a8b7c5;">'
                + '<p>' + inlineMarkdown(quoteLines.join(" ")) + '</p></blockquote>')
            continue
        }

        // Simple ordered and unordered lists.
        var unordered = /^\s{0,3}[-*+]\s+/.test(line)
        var ordered = /^\s{0,3}\d+[.)]\s+/.test(line)
        if (unordered || ordered) {
            var listItems = []
            var listPattern = unordered
                ? /^\s{0,3}[-*+]\s+(.*)$/
                : /^\s{0,3}\d+[.)]\s+(.*)$/
            while (i < lines.length) {
                var item = lines[i].match(listPattern)
                if (!item)
                    break
                listItems.push('<li>' + inlineMarkdown(item[1]) + '</li>')
                i++
            }
            output.push((ordered ? '<ol>' : '<ul>')
                + listItems.join("") + (ordered ? '</ol>' : '</ul>'))
            continue
        }

        // Paragraph: join wrapped Markdown source lines until the next block.
        var paragraph = [line.trim()]
        i++
        while (i < lines.length && lines[i].trim()
                && !isBlockStart(lines, i)) {
            paragraph.push(lines[i].trim())
            i++
        }
        output.push('<p>' + inlineMarkdown(paragraph.join(" ")) + '</p>')
    }

    return '<html><body style="color:#dce4eb;">'
        + output.join("\n")
        + '</body></html>'
}
