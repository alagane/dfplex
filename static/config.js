/*
 * config.js
 * part of the Web Fortress frontend.
 * Copyright (c) 2014 mifki, ISC license.
 *
 * This config file is directly executed, so be careful!
 * any value defined here becomes the default, which can be overrided via query
 * string.
 */

// note that some fields are edited by config-srv, which is
// dynamically-generated.
var config = {
	port: '1234',
	protocol: 'DFPlex-invalid',
	tiles: "Phoebus.png",
	size: 16,
	text: "ShizzleClean.png",
	overworld: "ShizzleClean.png",
	nick: "",
	secret: "",
	colors: undefined,
	hide_chat: undefined,
	show_fps: undefined
};
