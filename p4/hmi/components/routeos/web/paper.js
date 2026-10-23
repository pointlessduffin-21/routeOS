/* ============================================================
   PAPER — shared page behaviour
   No dependencies. Without this script the pages stay readable.
   The 3D layer lives in scenes-kit.js and does not depend on this file.

   Motion, stagger, phase labels, and numeric sort keys mirror
   kit/paper-policy.mjs. Keep the marked functions in sync.
   ============================================================ */
(function () {
    'use strict';

    var root = document.documentElement;
    root.classList.add('js');

    function closest(target, selector) {
        var node = target && target.nodeType === 1 ? target : target && target.parentElement;
        return node && node.closest ? node.closest(selector) : null;
    }

    var reduced = window.matchMedia('(prefers-reduced-motion: reduce)');
    var MOTION_KEY = 'paper-reduce-motion';
    var EFFECTS_KEY = 'paper-reduce-effects';

    /* mirrored in paper-policy.mjs */
    var PHASE_LABEL = {
        idle: '',
        submitting: 'Requesting',
        accepted: 'Request accepted; verification pending',
        verified: 'Verified',
        failed: 'Failed',
        unknown: 'Unknown outcome. Reconcile this request before retrying.'
    };

    function motionReduced() {
        return reduced.matches || root.getAttribute('data-paper-motion') === 'reduce';
    }
    function effectsReduced() {
        return root.getAttribute('data-paper-effects') === 'reduce';
    }

    function readStore(key) {
        try { return window.localStorage.getItem(key); } catch (err) { return null; }
    }
    function writeStore(key, value) {
        try { window.localStorage.setItem(key, value); } catch (err) { /* private mode */ }
    }

    /* mirrored in paper-policy.mjs */
    function parseStrictNumber(raw) {
        var text = String(raw == null ? '' : raw).trim();
        if (!/^[+-]?(?:\d+(?:\.\d+)?|\.\d+)$/.test(text)) return null;
        var value = Number(text);
        if (!Number.isFinite(value)) return null;
        return Object.is(value, -0) ? 0 : value;
    }

    /* mirrored in paper-policy.mjs */
    function compareCell(a, b, type, direction) {
        var dir = direction === 'descending' ? -1 : 1;
        if (type === 'number') {
            var na = parseStrictNumber(a);
            var nb = parseStrictNumber(b);
            if (na === null && nb === null) return 0;
            if (na === null) return 1;
            if (nb === null) return -1;
            return (na - nb) * dir;
        }
        return String(a).localeCompare(String(b), undefined, { numeric: true, sensitivity: 'base' }) * dir;
    }

    function phaseLabel(phase) {
        return Object.prototype.hasOwnProperty.call(PHASE_LABEL, phase) ? PHASE_LABEL[phase] : '';
    }

    var lastAnnounce = { key: '', at: 0 };
    function announce(message, id) {
        if (!message) return;
        var now = Date.now();
        var key = String(id || '') + '|' + message;
        if (key === lastAnnounce.key && now - lastAnnounce.at < 1000) return;
        lastAnnounce = { key: key, at: now };
        var region = document.getElementById('paper-status');
        if (!region) {
            region = document.createElement('div');
            region.id = 'paper-status';
            region.className = 'sr-only';
            region.setAttribute('role', 'status');
            region.setAttribute('aria-live', 'polite');
            region.setAttribute('aria-atomic', 'true');
            document.body.appendChild(region);
        }
        region.textContent = '';
        region.textContent = message;
    }

    function acknowledge(button, message) {
        if (!button) return;
        var text = message || PHASE_LABEL.submitting;
        button.setAttribute('aria-busy', 'true');
        var host = button.parentElement;
        var status = host ? host.querySelector('[data-paper-inline-status]') : null;
        if (!status) {
            status = document.createElement('span');
            status.className = 'inline-status';
            status.setAttribute('data-paper-inline-status', '');
            button.insertAdjacentElement('afterend', status);
        }
        status.textContent = text;
        announce(text);
    }

    function setFlag(name, on) {
        if (on) root.setAttribute(name, 'reduce');
        else root.removeAttribute(name);
    }

    function syncToggles() {
        var motion = motionReduced();
        var effects = effectsReduced();
        document.querySelectorAll('[data-paper-reduce-motion]').forEach(function (btn) {
            btn.setAttribute('aria-pressed', motion ? 'true' : 'false');
        });
        document.querySelectorAll('[data-paper-reduce-effects]').forEach(function (btn) {
            btn.setAttribute('aria-pressed', effects ? 'true' : 'false');
        });
    }

    function setReduceMotion(on) {
        setFlag('data-paper-motion', on);
        writeStore(MOTION_KEY, on ? '1' : '0');
        syncToggles();
    }
    function setReduceEffects(on) {
        setFlag('data-paper-effects', on);
        writeStore(EFFECTS_KEY, on ? '1' : '0');
        syncToggles();
    }

    if (readStore(MOTION_KEY) === '1') setFlag('data-paper-motion', true);
    if (readStore(EFFECTS_KEY) === '1') setFlag('data-paper-effects', true);

    var params;
    try { params = new URLSearchParams(window.location.search); } catch (err) { params = null; }
    if (params && params.get('motion') === 'reduce') setFlag('data-paper-motion', true);
    if (params && params.get('effects') === 'reduce') setFlag('data-paper-effects', true);

    reduced.addEventListener('change', function () {
        syncToggles();
        if (reduced.matches) announce('Reduced motion is on. Movement is limited to fades.');
    });

    document.addEventListener('click', function (e) {
        var motionBtn = closest(e.target, '[data-paper-reduce-motion]');
        if (motionBtn) {
            if (reduced.matches) {
                announce('System reduced motion stays on.');
                syncToggles();
            } else {
                setReduceMotion(root.getAttribute('data-paper-motion') !== 'reduce');
                announce(motionReduced() ? 'Reduced motion is on. Movement is limited to fades.' : 'Motion follows the system setting.');
            }
        }
        var effectsBtn = closest(e.target, '[data-paper-reduce-effects]');
        if (effectsBtn) {
            setReduceEffects(root.getAttribute('data-paper-effects') !== 'reduce');
            announce(effectsReduced() ? 'Visual effects are reduced.' : 'Visual effects are on.');
        }
        var stateBtn = closest(e.target, '[data-paper-state]');
        if (stateBtn && !stateBtn.disabled) {
            var turningOn = stateBtn.getAttribute('data-state') !== 'on';
            setState(stateBtn, turningOn);
            if (stateBtn.hasAttribute('aria-pressed')) {
                stateBtn.setAttribute('aria-pressed', turningOn ? 'true' : 'false');
            }
            var controlledId = stateBtn.getAttribute('aria-controls');
            var controlled = controlledId && document.getElementById(controlledId);
            if (controlled && stateBtn.getAttribute('data-paper-state') === 'password') {
                if (controlled.type === 'password' || controlled.type === 'text') {
                    controlled.type = turningOn ? 'text' : 'password';
                }
                stateBtn.textContent = turningOn ? 'Hide password' : 'Show password';
            }
        }
        var ack = closest(e.target, '[data-paper-ack]');
        if (ack && !ack.disabled) {
            if (ack.getAttribute('aria-busy') === 'true' && ack.hasAttribute('data-paper-once')) {
                e.preventDefault();
                e.stopPropagation();
                return;
            }
            acknowledge(ack, ack.getAttribute('data-paper-ack') || PHASE_LABEL.submitting);
        }
    });

    /* ---------- nav: scrolled state + mobile sheet ---------- */
    var nav = document.getElementById('nav');
    var toggle = document.getElementById('navToggle');
    var inertTargets = [];

    function onScroll() {
        if (nav) nav.classList.toggle('is-scrolled', window.scrollY > 12);
    }
    window.addEventListener('scroll', onScroll, { passive: true });
    onScroll();

    function focusable(scope) {
        return Array.prototype.filter.call(
            scope.querySelectorAll('a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])'),
            function (el) { return !el.closest('[hidden]') && el.getAttribute('aria-hidden') !== 'true'; }
        );
    }

    function setInert(on) {
        inertTargets.forEach(function (el) {
            if (on) el.setAttribute('inert', '');
            else el.removeAttribute('inert');
        });
    }

    function setOpen(open) {
        if (!nav || !toggle) return;
        nav.classList.toggle('is-open', open);
        toggle.setAttribute('aria-expanded', String(open));
        toggle.setAttribute('aria-label', open ? 'Close menu' : 'Open menu');
        document.body.classList.toggle('nav-locked', open);
        setInert(open);
        if (open) {
            var links = nav.querySelectorAll('.nav__links a, .nav__links button');
            if (links[0]) links[0].focus();
        }
    }

    if (toggle && nav) {
        inertTargets = Array.prototype.filter.call(
            document.querySelectorAll('main, footer'),
            function (el) { return !nav.contains(el); }
        );
        toggle.addEventListener('click', function () {
            setOpen(!nav.classList.contains('is-open'));
        });
        document.addEventListener('keydown', function (e) {
            if (!nav.classList.contains('is-open')) return;
            if (e.key === 'Escape') {
                setOpen(false);
                toggle.focus();
                return;
            }
            if (e.key !== 'Tab') return;
            var items = focusable(nav);
            if (!items.length) return;
            var first = items[0];
            var last = items[items.length - 1];
            if (e.shiftKey && document.activeElement === first) {
                e.preventDefault();
                last.focus();
            } else if (!e.shiftKey && document.activeElement === last) {
                e.preventDefault();
                first.focus();
            }
        });
        nav.querySelectorAll('.nav__links a').forEach(function (a) {
            a.addEventListener('click', function () { setOpen(false); });
        });
        window.matchMedia('(min-width: 901px)').addEventListener('change', function (m) {
            if (m.matches) setOpen(false);
        });
    }

    /* ---------- motion helpers (window.paper) ----------
       Every Paper page uses these. A page with none of them does not
       follow the skill.
         paper.enter(el)     play the entrance now
         paper.reveal(el)    same fade, for a node that just appeared
         paper.section(el)   mark data-section and play the longer fade
         paper.setState(el, 'on' | 'off' | name)
       Markup: data-enter, data-reveal, data-section, data-paper-state.
       data-motion="off" opts a subtree out. Alarms stay visible.
       Reduced motion still runs the fade: --paper-shift is 0. */
    var STAGGER_STEP = 40;
    var STAGGER_CAP = 240;

    function motionOff(el) {
        return !el || el.getAttribute('data-motion') === 'off' || closest(el, '[data-motion="off"]');
    }
    function holdsAlarm(el) {
        return el.matches('.status-critical, .banner, [role="alert"], .error') ||
            !!el.querySelector('.status-critical, [role="alert"]');
    }
    function motionNodes(selector) {
        return Array.prototype.filter.call(document.querySelectorAll(selector), function (el) {
            return !motionOff(el) && !holdsAlarm(el);
        });
    }
    function unique(list) {
        var seen = [];
        list.forEach(function (el) { if (seen.indexOf(el) < 0) seen.push(el); });
        return seen;
    }
    function capDelay(el, index) {
        var authored = el.style.getPropertyValue('--d');
        if (authored) {
            var ms = parseFloat(authored) * (/ms/i.test(authored) ? 1 : 1000);
            if (ms > STAGGER_CAP) el.style.setProperty('--d', STAGGER_CAP + 'ms');
            return;
        }
        var delay = index < 6 ? Math.min(STAGGER_CAP, index * STAGGER_STEP) : 0;
        el.style.setProperty('--d', delay + 'ms');
    }
    function playIn(el) {
        if (motionOff(el) || holdsAlarm(el)) return el;
        el.classList.add('is-motion');
        el.classList.remove('is-in');
        void el.offsetWidth;
        el.classList.add('is-in');
        return el;
    }
    function enter(el) { return playIn(el); }
    function reveal(el) { return playIn(el); }
    function section(el) {
        if (el && !motionOff(el)) el.setAttribute('data-section', '');
        return playIn(el);
    }
    function setState(el, state) {
        if (!el) return el;
        var name = state === true ? 'on' : state === false ? 'off' : String(state);
        el.setAttribute('data-state', name);
        el.classList.toggle('is-on', name === 'on');
        return el;
    }

    var enters = unique(motionNodes('[data-enter], .page-hero .eyebrow, .page-hero .display, .page-hero .lead, .page-hero .btns, .hero__title, .hero__foot, .login-card'));
    var staged = unique(motionNodes('[data-reveal], [data-section], .card, section.section, .pane'));
    enters.forEach(function (el, i) { capDelay(el, i); playIn(el); });
    staged.forEach(function (el, i) {
        if (enters.indexOf(el) !== -1) return;
        capDelay(el, i);
        el.classList.add('is-motion');
    });
    if ('IntersectionObserver' in window) {
        var io = new IntersectionObserver(function (entriesObserved) {
            entriesObserved.forEach(function (entry) {
                if (!entry.isIntersecting) return;
                entry.target.classList.add('is-in');
                io.unobserve(entry.target);
            });
        }, { rootMargin: '0px 0px -8% 0px', threshold: 0.08 });
        staged.forEach(function (el) {
            if (enters.indexOf(el) !== -1) return;
            io.observe(el);
        });
    } else {
        staged.forEach(function (el) { el.classList.add('is-in'); });
    }
    setTimeout(function () {
        document.querySelectorAll('.is-motion:not(.is-in)').forEach(function (el) {
            var rect = el.getBoundingClientRect();
            if (rect.top < window.innerHeight && rect.bottom > 0) el.classList.add('is-in');
        });
    }, 800);

    /* ---------- meters read their width from data-pct ---------- */
    document.querySelectorAll('.meter__fill[data-pct]').forEach(function (el) {
        el.style.setProperty('--pct', el.dataset.pct + '%');
    });

    /* Counters are not tweened. Intermediate digits would invent readings.
       The authored text is the value. */

    /* ---------- footer year ---------- */
    var year = document.getElementById('year');
    if (year) year.textContent = String(new Date().getFullYear());

    /* Title is already visible. is-loaded remains for older page CSS. */
    document.body.classList.add('is-loaded');

    /* ---------- native dialogs ---------- */
    function openDialog(dlg, opener) {
        if (!dlg || typeof dlg.showModal !== 'function') return;
        dlg.__paperOpener = opener || null;
        if (!dlg.open) dlg.showModal();
        var initial = dlg.querySelector('[data-paper-initial-focus], h1, h2');
        if (initial) {
            if (!initial.hasAttribute('tabindex')) initial.setAttribute('tabindex', '-1');
            initial.focus();
        }
    }
    document.querySelectorAll('dialog').forEach(function (dlg) {
        dlg.addEventListener('close', function () {
            var opener = dlg.__paperOpener;
            if (opener && opener.isConnected) opener.focus();
        });
    });
    document.addEventListener('click', function (e) {
        var closer = closest(e.target, '[data-paper-dialog-close]');
        if (closer) {
            var parent = closer.closest('dialog');
            if (parent && parent.open && typeof parent.close === 'function') parent.close();
        }
        var opener = closest(e.target, '[data-paper-dialog]');
        if (!opener) return;
        var dlg = document.getElementById(opener.getAttribute('data-paper-dialog'));
        if (!dlg) return;
        e.preventDefault();
        openDialog(dlg, opener);
    });

    /* ---------- local forms: errors stay associated with fields ---------- */
    document.querySelectorAll('form[data-paper-form]').forEach(function (form) {
        form.noValidate = true;
    });
    document.addEventListener('submit', function (e) {
        var form = e.target;
        if (!form || !form.hasAttribute || !form.hasAttribute('data-paper-form')) return;
        var fields = form.querySelectorAll('input, select, textarea');
        Array.prototype.forEach.call(fields, function (field) {
            if (!field.hasAttribute('data-paper-number')) return;
            var raw = field.value.trim();
            if (!raw) {
                field.setCustomValidity('');
                return;
            }
            var numeric = raw.split(/\s+/)[0];
            field.setCustomValidity(parseStrictNumber(numeric) === null
                ? 'Enter a decimal number, without grouping separators.'
                : '');
        });
        if (!form.checkValidity()) {
            e.preventDefault();
            Array.prototype.forEach.call(fields, function (field) {
                var bad = !field.checkValidity();
                if (bad) field.setAttribute('aria-invalid', 'true');
                else field.removeAttribute('aria-invalid');
                (field.getAttribute('aria-describedby') || '').split(/\s+/).forEach(function (id) {
                    var node = id && document.getElementById(id);
                    if (node && node.classList.contains('error')) {
                        node.hidden = !bad;
                        if (bad && field.validationMessage) node.textContent = field.validationMessage;
                    }
                });
            });
            var summary = form.querySelector('.error-summary');
            if (summary) {
                summary.hidden = false;
                if (!summary.hasAttribute('tabindex')) summary.setAttribute('tabindex', '-1');
                summary.focus();
            }
            announce('Check the highlighted fields.');
            return;
        }
        if (form.hasAttribute('data-paper-local')) {
            e.preventDefault();
            var note = form.querySelector('[data-paper-draft-note]');
            if (note) {
                note.hidden = false;
                var stamp = note.querySelector('time');
                if (stamp) {
                    var iso = new Date().toISOString();
                    stamp.setAttribute('datetime', iso);
                    stamp.textContent = iso;
                }
            }
            announce('Draft saved on this page. Nothing was sent.');
            writeDraft(form);
        }
    }, true);

    /* ---------- sortable tables. Missing numbers sort last, not as zero. ---------- */
    document.querySelectorAll('table[data-paper-sort]').forEach(function (table) {
        table.querySelectorAll('thead button[data-sort]').forEach(function (btn) {
            btn.addEventListener('click', function () {
                var th = btn.closest('th');
                if (!th || !th.parentNode) return;
                var index = Array.prototype.indexOf.call(th.parentNode.children, th);
                var type = btn.getAttribute('data-sort-type') || 'text';
                var dir = btn.getAttribute('aria-sort') === 'ascending' ? 'descending' : 'ascending';
                table.querySelectorAll('thead button[data-sort]').forEach(function (other) {
                    other.removeAttribute('aria-sort');
                });
                btn.setAttribute('aria-sort', dir);
                var tbody = table.querySelector('tbody');
                if (!tbody) return;
                var rows = Array.prototype.slice.call(tbody.querySelectorAll('tr'));
                rows.sort(function (a, b) {
                    var ac = a.children[index];
                    var bc = b.children[index];
                    var av = ac ? (ac.getAttribute('data-value') != null ? ac.getAttribute('data-value') : ac.textContent) : '';
                    var bv = bc ? (bc.getAttribute('data-value') != null ? bc.getAttribute('data-value') : bc.textContent) : '';
                    return compareCell(av, bv, type, dir);
                });
                rows.forEach(function (row) { tbody.appendChild(row); });
            });
        });
    });

    /* ---------- tabs: arrow keys when a real tablist is marked ---------- */
    document.querySelectorAll('[role="tablist"]').forEach(function (list) {
        var tabs = Array.prototype.slice.call(list.querySelectorAll('[role="tab"]'));
        tabs.forEach(function (tab) {
            var panel = document.getElementById(tab.getAttribute('aria-controls'));
            if (panel) panel.hidden = tab.getAttribute('aria-selected') !== 'true';
        });
        function activate(tab) {
            tabs.forEach(function (item) {
                var on = item === tab;
                item.setAttribute('aria-selected', on ? 'true' : 'false');
                item.tabIndex = on ? 0 : -1;
                var panel = document.getElementById(item.getAttribute('aria-controls'));
                if (panel) panel.hidden = !on;
            });
            tab.focus();
        }
        list.addEventListener('keydown', function (e) {
            var current = tabs.indexOf(document.activeElement);
            if (current < 0) return;
            var next = null;
            if (e.key === 'ArrowRight' || e.key === 'ArrowDown') next = tabs[(current + 1) % tabs.length];
            else if (e.key === 'ArrowLeft' || e.key === 'ArrowUp') next = tabs[(current - 1 + tabs.length) % tabs.length];
            else if (e.key === 'Home') next = tabs[0];
            else if (e.key === 'End') next = tabs[tabs.length - 1];
            if (!next) return;
            e.preventDefault();
            activate(next);
        });
        tabs.forEach(function (tab) {
            tab.addEventListener('click', function () { activate(tab); });
        });
    });

    syncToggles();

    /* Named ledger views, a local export, and a tab-scoped draft. */
    var PRESET_NOTE = {
        all: 'All assets in this ledger.',
        attention: 'Assets that need attention: critical, offline, or missing data.',
        running: 'Assets whose state says Running.'
    };
    function applyPreset(group, name) {
        var table = document.getElementById(group.getAttribute('data-paper-presets'));
        if (!table || !table.tBodies[0]) return;
        Array.prototype.forEach.call(table.tBodies[0].rows, function (row) {
            var state = row.cells[1] ? row.cells[1].textContent : '';
            var show = name === 'all'
                || (name === 'attention' && /Critical|Offline|Unavailable|Disconnected/i.test(state))
                || (name === 'running' && /Running/i.test(state));
            row.hidden = !show;
        });
        group.querySelectorAll('[data-paper-preset]').forEach(function (btn) {
            btn.setAttribute('aria-pressed', btn.getAttribute('data-paper-preset') === name ? 'true' : 'false');
        });
        var noteId = group.getAttribute('aria-describedby');
        var note = noteId && document.getElementById(noteId);
        if (note && PRESET_NOTE[name]) note.textContent = PRESET_NOTE[name];
    }
    document.querySelectorAll('[data-paper-presets]').forEach(function (group) {
        group.addEventListener('click', function (e) {
            var btn = closest(e.target, '[data-paper-preset]');
            if (!btn) return;
            applyPreset(group, btn.getAttribute('data-paper-preset') || 'all');
        });
    });
    document.querySelectorAll('table[data-paper-sort]').forEach(function (table) {
        if (table.tBodies[0]) table.tBodies[0].__paperOrder = Array.prototype.slice.call(table.tBodies[0].rows);
    });
    function draftKey(form) {
        return 'paper-draft:' + location.pathname + '#' + (form.id || 'form');
    }
    function writeDraft(form) {
        if (!form || !form.hasAttribute('data-paper-persist')) return;
        var data = {};
        Array.prototype.forEach.call(form.elements, function (field) {
            if (!field.name || field.type === 'password' || field.type === 'file') return;
            data[field.name] = field.value;
        });
        try { sessionStorage.setItem(draftKey(form), JSON.stringify(data)); } catch (err) { /* storage blocked */ }
    }
    function clearDraft(form) {
        try { sessionStorage.removeItem(draftKey(form)); } catch (err) { /* storage blocked */ }
        Array.prototype.forEach.call(form.elements, function (field) {
            if (!field.name || field.type === 'password' || field.type === 'file') return;
            field.value = '';
            field.removeAttribute('aria-invalid');
        });
        var note = form.querySelector('[data-paper-draft-note]');
        if (note) note.hidden = true;
        announce('Draft discarded on this page. Nothing was sent.');
    }
    document.querySelectorAll('form[data-paper-persist]').forEach(function (form) {
        var raw = null;
        try { raw = sessionStorage.getItem(draftKey(form)); } catch (err) { raw = null; }
        if (!raw) return;
        var data;
        try { data = JSON.parse(raw); } catch (err) { return; }
        var restored = false;
        Object.keys(data).forEach(function (name) {
            var field = form.elements[name];
            if (!field || field.type === 'password') return;
            field.value = data[name];
            restored = true;
        });
        if (!restored) return;
        var note = form.querySelector('[data-paper-draft-note]');
        if (note) note.hidden = false;
        announce('Draft restored on this page. Nothing was sent.');
    });
    document.addEventListener('click', function (e) {
        var exp = closest(e.target, '[data-paper-export]');
        if (exp) {
            var table = document.getElementById(exp.getAttribute('data-paper-export'));
            if (!table || !table.tBodies[0]) return;
            var lines = [];
            Array.prototype.forEach.call(table.tBodies[0].rows, function (row) {
                if (row.hidden) return;
                lines.push(Array.prototype.map.call(row.cells, function (cell) {
                    return cell.textContent.replace(/\s+/g, ' ').trim();
                }).join('\t'));
            });
            var blob = new Blob([lines.join('\n') + '\n'], { type: 'text/plain' });
            var url = URL.createObjectURL(blob);
            var link = document.createElement('a');
            link.href = url;
            link.download = 'ledger.txt';
            document.body.appendChild(link);
            link.click();
            link.remove();
            URL.revokeObjectURL(url);
            announce('Exported the visible rows. Nothing was sent.');
        }
        var restore = closest(e.target, '[data-paper-restore]');
        if (restore) {
            var restoredTable = document.getElementById(restore.getAttribute('data-paper-restore'));
            if (restoredTable && restoredTable.tBodies[0] && restoredTable.tBodies[0].__paperOrder) {
                restoredTable.tBodies[0].__paperOrder.forEach(function (row) {
                    restoredTable.tBodies[0].appendChild(row);
                });
                restoredTable.querySelectorAll('[aria-sort]').forEach(function (node) {
                    node.removeAttribute('aria-sort');
                });
            }
            document.querySelectorAll('[data-paper-presets]').forEach(function (group) {
                if (!restoredTable || group.getAttribute('data-paper-presets') === restoredTable.id) applyPreset(group, 'all');
            });
            announce('Restored the authored order.');
        }
        var discard = closest(e.target, '[data-paper-discard]');
        if (discard) {
            var form = closest(discard, 'form');
            if (form) clearDraft(form);
        }
    });

    window.paper = {
        motionReduced: motionReduced,
        effectsReduced: effectsReduced,
        setReduceMotion: setReduceMotion,
        setReduceEffects: setReduceEffects,
        announce: announce,
        phaseLabel: phaseLabel,
        acknowledge: acknowledge,
        openDialog: openDialog,
        parseStrictNumber: parseStrictNumber,
        compareCell: compareCell,
        enter: enter,
        reveal: reveal,
        section: section,
        setState: setState
    };
})();
