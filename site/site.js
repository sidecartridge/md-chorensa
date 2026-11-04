/* The developer details: the switch in the top bar sets data-dev on <html> (the stylesheet
   hides the developer blocks when off), keeps the choice for the next visit and puts it in the
   address, so a link opens with the details shown. The script in the page's head applies it
   before the page draws; without scripts the switch stays hidden and every detail shows. */
(function () {
  'use strict';
  var root = document.documentElement;
  var button = document.querySelector('.dev-switch');
  if (!button) return;
  function show(on) {
    root.dataset.dev = on ? 'on' : 'off';
    button.setAttribute('aria-checked', on ? 'true' : 'false');
  }
  button.addEventListener('click', function () {
    var on = root.dataset.dev !== 'on';
    show(on);
    try { localStorage.setItem('crs-dev', on ? '1' : '0'); } catch (e) { /* this visit only */ }
    var url = new URL(location.href);
    if (on) url.searchParams.set('dev', '1');
    else url.searchParams.delete('dev');
    history.replaceState(null, '', url);
  });
  show(root.dataset.dev === 'on');
  button.hidden = false;
})();

/* The video: the GIF until "Watch" is pressed. The button is a link to the video file, so
   without scripts it opens it; with them it puts a player, playing with its sound, in the GIF's
   place. The file is the site's own: nothing loads from another site. */
(function () {
  'use strict';
  var figure = document.getElementById('video');
  if (!figure) return;
  var link = figure.querySelector('.watch');
  link.addEventListener('click', function (e) {
    e.preventDefault();
    var video = document.createElement('video');
    video.src = link.getAttribute('href');
    video.poster = 'images/play-poster.png';
    video.controls = true;
    video.autoplay = true;
    video.playsInline = true;
    video.width = 512;
    video.height = 512;
    figure.querySelector('.frame').replaceWith(video);
    video.focus();
  });
})();
