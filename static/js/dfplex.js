/*
 * dfplex.js
 * Copyright (c) 2014 mifki, ISC license.
 */

/*jslint browser:true */

var params = getParams();
// TODO: tag colors
const colors = [
	32, 39, 49,
	0, 106, 255,
	68, 184, 57,
	114, 156, 251,
	212, 54, 85,
	176, 50, 255,
	217, 118, 65,
	170, 196, 178,
	128, 151, 156,
	48, 165, 255,
	144, 255, 79,
	168, 212, 255,
	255, 82, 82,
	255, 107, 255,
	255, 232, 102,
	255, 250, 232
];
const NUMBER_OF_COLORS = 16;
const GRID_SIZE = 16
const MAX_FPS = 20;

var port = params.port;
var protocol = params.protocol;
var tileSet = params.tiles;
var textSet = params.text;
var ovrSet = params.overworld;
var colorscheme = params.colors;
var nick = params.nick;
var secret = params.secret;
var tileSize = params.size;

var wsUri = 'ws://' + location.hostname + ':' + port +
	'/' + encodeURIComponent(nick) +
	'/' + encodeURIComponent(secret);
console.log(wsUri);
var active = false;
var lastFrame = 0;

var tilew = tileSize;
var tileh = tileSize;

var cmd = {
	"update":  110,
	"sendKey": 111,
	"connect": 115,
	"resize": 117,
	"requestTurn": 116
};

// Converts integer value in seconds to a time string, HH:MM:SS
function toTime(n) {
	var h = Math.floor(n / 60  / 60);
	var m = Math.floor(n / 60) % 60;
	var s = n % 60;
	return ("0" + h).slice(-2) + ":" +
	       ("0" + m).slice(-2) + ":" +
	       ("0" + s).slice(-2);
}

function plural(n, unit)
{
	return n + " " + unit + (n === 1 ? "" : "s");
}

// Converts an integer value in ticks to the dwarven calendar
function toGameTime(n) {
	var years = Math.floor(n / 12 / 28 / 1200);
	var months = Math.floor(n / 28 / 1200) % 12;
	var days = Math.floor(n / 1200) % 28;
	var ticks = n % 1200;

	var times = [];
	if (years > 0) {
		times.push(plural(years, "year"));
	}
	if (months > 0) {
		times.push(plural(months, "month"));
	} else if (days > 0) {
		times.push(plural(days, "day"));
	} else {
		times.push(plural(ticks, "tick"));
	}

	return times.join(", ");
}

function setStats(userCount, loadframes) {
	var u = document.getElementById('user-count');
	// var t = document.getElementById('time-left');
	u.innerHTML = String(userCount) + " <i class='fa fa-users'></i> ";
}

function setDebug(debugInfo) {
	var c = document.getElementById('trace-container');
	var u = document.getElementById('trace');
	if (debugInfo == "")
	{
		u.innerHTML = "";
		//c.setAttribute("style","width:0px");
	}
	else
	{
		var ih = "<pre>" + debugInfo + "</pre>";
		if (u.innerHTML != ih)
		{
			u.innerHTML = ih;
		}
		//c.setAttribute("style","width:200px");
	}
}

var statusOnClick = null;
function setStatus(text, color, onclick) {
	var m = document.getElementById('message');
	if (m.innerHTML != text)
	{
		m.innerHTML = text;
	}
	var st = m.parentNode;
	if (statusOnClick) {
		st.removeEventListener('click', statusOnClick);
	}
	statusOnClick = onclick;
	if (onclick) {
		st.addEventListener('click', onclick);
		st.style.cursor = 'pointer';
	} else {
		st.style.cursor = '';
	}
	st.style.backgroundColor = color;
}

function connect() {
	setStatus('Connecting...', 'orange');
	websocket = new WebSocket(wsUri, [protocol, 'DFPlex-invalid']);
	websocket.binaryType = 'arraybuffer';
	websocket.onopen  = onOpen;
	websocket.onclose = onClose;
	websocket.onerror = onError;
}

function onOpen(evt) {
	setStatus('Connected, initializing...', 'orange');

	websocket.send(new Uint8Array([cmd.connect]));

	websocket.send(new Uint8Array([cmd.update]));
	websocket.onmessage = onMessage;
}

var isError = false;
function onClose(evt) {
	console.log("Disconnect code #" + evt.code + ", reason: " + evt.reason);
	console.log(isError);
	if (isError) {
		isError = false;
		setStatus('Connection Error. Click to retry', 'red', connect);
	} else if (evt.reason) {
		setStatus(evt.reason + ' Click to try again.', 'red', connect);
	} else {
		setStatus('Unknown disconnect: Check the console (Ctrl-Shift-J), then click to reconnect.', 'red', connect);
	}
}

function onError(ev) {
	console.log("error triggered.");
	isError = true;
}

function requestTurn() {
	websocket.send(new Uint8Array([cmd.requestTurn]));
}

function renderQueueStatus(s) {
	var display = s.currentPlayer || "Connected.";
	active = true;
	var colour = 'yellow';
	if (display == "Connected." || display.startsWith("(MP)"))
		colour = 'green'
	if (display.startsWith("(UP)"))
		colour = 'grey'
	setStatus(display, colour, requestTurn);
	setStats(s.playerCount, s.load);
	setDebug(s.debugInfo);
}

// TODO: document, split
function renderUpdate(ctx, data, offset) {
	let t = []; // text tile indices
	let ovr = []; // overworld tile indices

	for (let k = offset; k < data.length; k += 5) {
		let dataSlice = data.slice(k, k + 5)

		drawBackground(ctx, dataSlice);

		// 6th bit: text
		if ((dataSlice[3] & 64)) {
			t.push(k);
			continue;
		}
		// 7th bit: overworld
		if ((dataSlice[3] & 128)) {
			ovr.push(k);
			continue;
		}

		drawForeground(ctx, dataSlice, ts.width, ts.height, cd);
	}

	// draw text
	for (let k = 0; k < t.length; k++) {
		let offset = t[k];
		let dataSlice = data.slice(offset, offset + 5)

		drawForeground(ctx, dataSlice, tt.width, tt.height, ct);
	}

	// draw overwold
	for (let k = 0; k < ovr.length; k++) {
		let offset = ovr[k];
		let dataSlice = data.slice(offset, offset + 5)

		drawForeground(ctx, dataSlice, tovr.width, tovr.height, covr);
	}
}

function drawBackground(ctx, dataSlice) {
	let x = dataSlice[0];
	let y = dataSlice[1];
	let bgColor = dataSlice[3] % 0xf;

	let bg_x = bgColor * ts.width + (GRID_SIZE - 1) * ts.width / GRID_SIZE;
	let bg_y = 15 * ts.height / GRID_SIZE;
		ctx.drawImage(
		cd,
		bg_x, bg_y, ts.width / GRID_SIZE, ts.height / GRID_SIZE,
			x * tilew, y * tileh, tilew, tileh
		);
	}
	
function drawForeground(ctx, dataSlice, width, height, image) {
	let x = dataSlice[0];
	let y = dataSlice[1];
	let s = dataSlice[2];
	let fgColor = dataSlice[4];

	let fg_x = fgColor * width + (s & 0x0F) * width / GRID_SIZE;
	let fg_y = (s >> 4) * height / GRID_SIZE;
		ctx.drawImage(
		image,
		fg_x, fg_y, width / GRID_SIZE, height / GRID_SIZE,
			x * tilew, y * tileh, tilew, tileh
		);
	}
}

function updateCanvasDOM() {
	canvas.style.width = "";
	canvas.style.height = "";
	
	var maxw = canvas.parentNode.offsetWidth;
	var maxh = canvas.parentNode.offsetHeight - document.getElementById('status-id').offsetHeight;
	
	console.log(canvas.width + "x" + canvas.height + " in " + maxw + "x" + maxh);
		
	if (maxw >= canvas.width && maxh >= canvas.height) {
		canvas.style.left = (maxw - canvas.width) / 2 + "px";
		return
	}
		
	var aspectRatio = canvas.width / canvas.height;
	var constrainWidth = (maxw / maxh < aspectRatio);
	
	console.log("constrainWidth: " + constrainWidth)
	canvas.style.left = "0"
	
	if (constrainWidth) {
	   canvas.style.width  = "100%";
	   canvas.style.height = ""
	} else {
	   canvas.style.width  = "";
	   canvas.style.height = "100%";
   }
}

function onMessage(evt) {
	var data = new Uint8Array(evt.data);

	var ctx = canvas.getContext('2d');
	if (data[0] === cmd.update) {
		if (stats) { stats.begin(); }
		var gameStatus = {};
		gameStatus.playerCount = data[1] & 127;

		gameStatus.isActive = (data[2] & 1) !== 0;
		
		// removed
		gameStatus.isNoPlayer = false;
		gameStatus.ingameTime = false;
		gameStatus.timeLeft = -1;
		
		// # of frames overall
		gameStatus.load =
			(data[3]<<0) |
			(data[4]<<8) |
			(data[5]<<16) |
			(data[6]<<24);

		var neww = data[7] * tilew;
		var newh = data[8] * tileh;
		if (neww != canvas.width || newh != canvas.height) {
			canvas.width = neww;
			canvas.height = newh;
			
			updateCanvasDOM()
		}

		var nickSize = data[9];
		// this only works because we know the input is uri-encoded ascii
		var activeNick = "";
		for (var i = 10; (i < 10 + nickSize) && data[i] !== 0; i++) {
			activeNick += String.fromCharCode(data[i]);
		}
		gameStatus.currentPlayer = decodeURIComponent(activeNick);
		
		var offset = 10 + nickSize;
		
		var debugInfoSize = data[offset] | (data[offset + 1] << 8);
		offset += 2;
		var debugInfo = ""
		for (var i = 0; i < debugInfoSize && data[offset + i] !== 0; ++i) {
			debugInfo += String.fromCharCode(data[offset + i]);
		}
		offset += debugInfoSize;
		gameStatus.debugInfo = debugInfo;

		renderQueueStatus(gameStatus);
		renderUpdate(ctx, data, offset);

		var now = performance.now();
		var nextFrame = (1000 / MAX_FPS) - (now - lastFrame);
		if (nextFrame < 4) {
			websocket.send(new Uint8Array([cmd.update]));
		} else {
			setTimeout(function() {
				websocket.send(new Uint8Array([cmd.update]));
			}, nextFrame);
		}
		lastFrame = performance.now();
		if (stats) { stats.end(); }
	}
}

// FIXME: tilewh-ify
function colorize(img, cnv) {
	var ctx3 = cnv.getContext('2d');

	for (var i = 0; i < NUMBER_OF_COLORS; i++) {
		ctx3.drawImage(img, i * img.width, 0);

		var idata = ctx3.getImageData(i * img.width, 0, img.width, img.height);
			var pixels = idata.data;

			for (var u = 0, len = pixels.length; u < len; u += 4) {
			pixels[u] = pixels[u] * (colors[i * 3 + 0] / 255);
			pixels[u + 1] = pixels[u + 1] * (colors[i * 3 + 1] / 255);
			pixels[u + 2] = pixels[u + 2] * (colors[i * 3 + 2] / 255);
			}
		ctx3.putImageData(idata, i * img.width, 0);

			ctx3.fillStyle = 'rgb(' +
				colors[i * 3 + 0] + ',' +
				colors[i * 3 + 1] + ',' +
				colors[i * 3 + 2] + ')';

		ctx3.fillRect(
			(i + (GRID_SIZE - 1) / GRID_SIZE) * img.width,
			img.height * (GRID_SIZE - 1) / GRID_SIZE,
			img.width / GRID_SIZE,
			img.height / GRID_SIZE
		);
	}
}

// Crazy closures, Batman, what's going on here?
var make_loader = function() {
	var loading = 0;
	return function() {
		loading += 1;
		return function() {
			loading -= 1;
			if (loading <= 0) {
				init();
			}
		};
	};
}();

var cd, ct, covr;
function init() {
	document.body.style.backgroundColor =
		'rgb(' + colors[0] + ',' + colors[1] + ',' + colors[2] + ')';

	cd = document.createElement('canvas');
	
	cd.width = ts.width * NUMBER_OF_COLORS;
	cd.height = ts.height;
	colorize(ts, cd);

	ct = document.createElement('canvas');
	ct.width = ts.width * NUMBER_OF_COLORS;
	ct.height = ts.height;
	colorize(tt, ct);
	
	covr = document.createElement('canvas');
	covr.width = ts.width * NUMBER_OF_COLORS;
	covr.height = ts.height;
	colorize(tovr, covr);

	lastFrame = performance.now();

	connect();
}

var stats;
if (params.show_fps) {
	stats = new Stats();
	document.body.appendChild(stats.domElement);
	stats.domElement.style.position = "absolute";
	stats.domElement.style.bottom = "0";
	stats.domElement.style.left   = "0";
}

function getFolder(path) {
	return path.substring(0, path.lastIndexOf('/') + 1);
}

var root = getFolder(window.location.pathname);

// tileset
var ts = document.createElement('img');
ts.src =  root + "art/" + tileSet;
ts.onload = make_loader();

// textset
var tt = document.createElement('img');
tt.src = root + "art/" + textSet;
tt.onload = make_loader();

// overworldset
var tovr = document.createElement('img');
tovr.src = root + "art/" + ovrSet;
tovr.onload = make_loader();

if (colorscheme !== undefined) {
	var colorReq = new XMLHttpRequest();
	var colorLoader = make_loader();
	colorReq.onload = function() {
		colors = JSON.parse(this.responseText);
		colorLoader();
	};
	colorReq.open("get", root + "colors/" + colorscheme);
	colorReq.send();
}


var canvas = document.getElementById('myCanvas');
canvas.style.position="absolute"
canvas.style.left="0"
canvas.style.top="0"
document.onkeydown = function(ev) {
	if (!active)
		return;

	if (ev.keyCode === 18 ||
	    ev.keyCode === 17 ||
        ev.keyCode === 16) {
		return;
	}

	var mod = ev.shiftKey | (ev.ctrlKey << 1) | (ev.altKey << 2);
	var charCode = 0;
	if (ev.key.length == 1){
	    charCode = ev.key.charCodeAt(0)
	}
	var data = new Uint8Array([cmd.sendKey, ev.keyCode, charCode, mod]);
	logKeyCode(ev);
	websocket.send(data);
	ev.preventDefault();
};

function udpateScreenSize() {
	var maxw = canvas.parentNode.offsetWidth;
	var maxh = canvas.parentNode.offsetHeight - document.getElementById('status-id').offsetHeight;
	var gridw = Math.min(Math.floor(maxw / tilew), 255);
	// need to subtract 1, likely the header.
	var gridh = Math.min(Math.floor(maxh / tileh), 255);
	
	// request resize from server.
	var data = new Uint8Array([cmd.resize, gridw, gridh]);
	
	// TODO: only send if screen size changed.
	websocket.send(data);
}

window.onresize = udpateScreenSize;
window.onload   = udpateScreenSize;

// this is fine.
setTimeout(udpateScreenSize, 10);
setTimeout(udpateScreenSize, 100);
setTimeout(udpateScreenSize, 200);
setTimeout(udpateScreenSize, 400);
setTimeout(udpateScreenSize, 800);
setInterval(udpateScreenSize, 3000);
