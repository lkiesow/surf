/* Disable popups (except for Google Drive) */
if (!( 
			window.location.href.match(/https*:\/\/drive.google.com\/.*/) ||
			window.location.href.match(/https*:\/\/docs.google.com\/.*/)
	  )) {
	window.open_bak = window.open;

	window.open = function(a,b,c) {
		/* TODO: Popup message in top right corner */
	};
}

function centerbody(e) {
	var e = window.event ? event : e;
	if ( e.ctrlKey && e.keyCode == 109 ) { /* 109 = m */
		body = document.getElementsByTagName('body')[0];
		body.setAttribute('style', 'max-width: 1000px; margin-left: auto; margin-right: auto;');
	}
}
document.onkeypress = centerbody

