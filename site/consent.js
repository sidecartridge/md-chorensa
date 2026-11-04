// The cookie banner of the SidecarTridge sites, as on jackintosh.sidecartridge.com and
// gemdrive.sidecartridge.com (from sidecartridge.com's cookie-consent.html). cookieconsent v2.9.2
// (https://github.com/orestbida/cookieconsent, MIT) is in vendor/. The consent cookie is shared with
// sidecartridge.com when the site is served there; the analytics script in the page's head runs only
// after consent, and only loads on *.sidecartridge.com.
    window.addEventListener('load', function () {

        // obtain plugin
        var cc = initCookieConsent();

        // run plugin with your configuration
        cc.run({
            cookie_domain: /(^|\.)sidecartridge\.com$/.test(location.hostname) ? 'sidecartridge.com' : location.hostname,
            current_lang: 'en',
            autoclear_cookies: true,                   // default: false
            page_scripts: true,                        // default: false

            onFirstAction: function (user_preferences, cookie) {
                // callback triggered only once on the first accept/reject action
            },

            onAccept: function (cookie) {
                // callback triggered on the first accept/reject action, and after each page load
            },

            onChange: function (cookie, changed_preferences) {
                // If analytics category is disabled => disable google analytics
                if (!cc.allowedCategory('analytics')) {
                    typeof gtag === 'function' && gtag('consent', 'update', {
                        'analytics_storage': 'denied'
                    });
                }
            },

            languages: {
                'en': {
                    consent_modal: {
                        title: 'We use cookies!',
                        description: 'Hi, this website uses essential cookies to ensure its proper operation and tracking cookies to understand how you interact with it. The latter will be set only after consent. <button type="button" data-cc="c-settings" class="cc-link">Let me choose</button>',
                        primary_btn: {
                            text: 'Accept all',
                            role: 'accept_all'              // 'accept_selected' or 'accept_all'
                        },
                        secondary_btn: {
                            text: 'Reject all',
                            role: 'accept_necessary'        // 'settings' or 'accept_necessary'
                        }
                    },
                    settings_modal: {
                        title: 'Cookie preferences',
                        save_settings_btn: 'Save settings',
                        accept_all_btn: 'Accept all',
                        reject_all_btn: 'Reject all',
                        close_btn_label: 'Close',
                        cookie_table_headers: [
                            { col1: 'Name' },
                            { col2: 'Domain' },
                            { col3: 'Expiration' },
                            { col4: 'Description' }
                        ],
                        blocks: [
                            {
                                title: 'Cookie usage 📢',
                                description: 'We use cookies to make the website work and, if you accept them, to count visits. You can turn each category on or off whenever you want. For more details relative to cookies and other sensitive data, please read the full <a href="https://sidecartridge.com/privacy-policy/" class="cc-link" target="_blank" rel="noopener">privacy policy</a>.'
                            }, {
                                title: 'Strictly necessary cookies',
                                description: 'These cookies are needed for the website to work, such as the one that remembers your choice here.',
                                toggle: {
                                    value: 'necessary',
                                    enabled: true,
                                    readonly: true          // cookie categories with readonly=true are all treated as "necessary cookies"
                                }
                            }, {
                                title: 'Performance and Analytics cookies',
                                description: 'Google Analytics: these cookies let us count visits and see which parts of the page are read. They are set only if you accept them.',
                                toggle: {
                                    value: 'analytics',     // your cookie category
                                    enabled: false,
                                    readonly: false
                                },
                                cookie_table: [             // list of all expected cookies
                                    {
                                        col1: '^_ga',       // match all cookies starting with "_ga"
                                        col2: 'sidecartridge.com',
                                        col3: '2 years',
                                        col4: 'Google Analytics: tells visits apart',
                                        is_regex: true
                                    },
                                    {
                                        col1: '_gid',
                                        col2: 'sidecartridge.com',
                                        col3: '1 day',
                                        col4: 'Google Analytics: tells visits apart for a day',
                                    }
                                ]
                            },
                            {
                                title: 'More information',
                                description: 'For any queries in relation to our policy on cookies and your choices, please <a class="cc-link" href="https://store.sidecartridge.com/pages/contact" target="_blank" rel="noopener">contact us</a>.',
                            }
                        ]
                    }
                }
            }
        });
    });
