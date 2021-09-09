#! /bin/awk -f

BEGIN {
	ret = 0
}

{
	if (/^commit /) {
		state = 1
		commit = $2
		is_merge = 0
	}
	else if (state==1 && /^$/) state = 2
	else if (state==2) state = 3
	else if (state==3) state = 4
}

state==1 && $1=="Merge:" {
	is_merge = 1
}

function check_module_name(mod) {
	if (mod ~ /^(win-capture|rtmp-services|obs-ffmpeg|CI|pipewire):$/)
		return
	if (mod ~ /^(libobs|deps|docs|UI)(|\/[a-z_-]*):$/)
		return
	if (mod ~ /^(obs-filters|obs-outputs|win-dshow|cmake|obs-browser):$/)
		return
	if (mod ~ /^(libobs-opengl|linux-capture|image-source|decklink):$/)
		return
	if (mod ~ /^(obs-transitions|enc-amf|frontend-tools):$/)
		return
	print "Info: not frequently appearing module name: "mod
}

state==3 && !is_merge {
	title = gensub(/^ */, "", 1, $0)
	if (title ~ /^Revert ".*"$/) {
		next
	}
	if (title ~ /^Merge [0-9a-f]{40} into [0-9a-f]{40}$/) {
		next
	}

	if (title ~ /^[A-Za-z0-9./-]+(, ?[A-Za-z0-9/-]+)*: /) {
		title1 = gensub(/^[^:]*: /, "", 1, title)
		mod = $1
		check_module_name(mod)
	} else {
		title1 = title
		mod = ""
	}

	split(title1, title_a, / /)
	if (title_a[1] !~ /^([A-Z][a-z]*(-[a-z]+)*|Don't)$/) {
		print "Error: commit "commit": first word: "title_a[1]
		ret = 1
	}

	if (length(title) > 72) {
		print "Error: commit "commit": too long title: "title
		ret = 1
	}
	else if (length(title1) > 50) {
		print "Warning: commit "commit": long title excluding module name: "title1
	}
}

state==4 {
	line = gensub(/^ */, "", 1, $0)
	if (length(line) > 72) {
		print "Error: commit "commit": too long description in a line: "line
		ret = 2
	}
}

END {
	exit(ret)
}
