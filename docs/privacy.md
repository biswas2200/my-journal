# Privacy policy: Journal Notes

Last changed: 1 November 2026

Journal Notes ("Journal & Notes") is a desktop app for Linux: a private
journal and a separate Notes app for study notes. It runs on your own
computer. There is no server of ours, no account with us, and nothing
you write is ever sent to the developer.

## What stays on your computer

- **The Journal** is offline. It has no network access at all: the
  program blocks itself from the network, and the system's AppArmor
  profile (installed with it) blocks it again from outside. Its entries
  are encrypted on your disk.
- **Notes** keeps your subjects, topics, notes and study time in
  `~/.local/share/journal-notes/` on your computer, with daily backup
  copies in the same folder. Notes cannot open the Journal's files.

Nothing below happens unless you turn it on yourself.

## Google Drive backup (optional)

If you choose **☁ Drive → Sign in with Google** in Notes, Google asks you
to allow Journal Notes to "see, edit, create and delete only the
specific Google Drive files you use with this app". This is Google's
`drive.file` permission. With it, Journal Notes:

- creates a folder named "Notes" in your My Drive, a folder per subject
  in it, and one Google Doc per topic, holding that topic's notes;
- replaces the text of those Docs when you back up again (Google keeps
  the earlier versions in each Doc's version history);
- reads those Docs back, to check they hold the same text as your notes;
- reads who can open those Docs, only to warn you if anyone besides you
  can;
- looks among the files it made itself, so it never makes one twice.

It **cannot see, open or change any other file** in your Google Drive:
Google does not allow it with this permission.

Your notes travel directly from your computer to Google over an
encrypted connection. The lasting part of your Google sign-in is kept
in your computer's login keyring; the short-lived (one hour) part only
in memory. **Sign out** in Notes asks Google to cancel the sign-in and
forgets it on your computer.

### Use of Google user data

Journal Notes uses Google user data only to back up your own notes to
your own Google Drive and to check those backups, as described above.
It does not use it for anything else, does not show advertising, does
not sell or give it to anyone, and does not use it to train AI models.
No person reads it. Journal Notes' use and transfer of information
received from Google APIs adheres to the
[Google API Services User Data Policy](https://developers.google.com/terms/api-services-user-data-policy),
including the Limited Use requirements.

## AI enhanced copy (optional)

If you choose **✦ Enhance** on a study session (Notes asks you first,
once), Notes sends to Google's Gemini model, in your own Google Cloud
project and with your own Google sign-in:

- that session's note and the names of its subject and topic;
- the text of the web pages you listed as its sources (Notes fetches
  them from those websites, so they see a request from your computer);
- a few short samples of your other notes, so the copy reads like you.

Google's Cloud terms for your project apply to that request. The
developer of Journal Notes receives none of it. Your note itself is
never changed: the enhanced copy is saved beside it, on your computer.

## Reporting a problem (optional)

**⚑ Report** opens a form on the project's GitHub page in your browser.
The form's address carries the app's version and your system
(distribution, kernel and GTK versions), so GitHub receives them when
the page opens. Nothing is posted until you fill in the form and submit
it on GitHub yourself, under your GitHub account; GitHub's privacy
statement applies there. No note or journal text is ever included.

## What Journal Notes never does

- No analytics, tracking, advertising or crash reports sent anywhere.
- No account with the developer, no server of the developer's.
- No sale or sharing of your data with anyone.

## Removing your data

- **From Google:** in Notes, **☁ Drive → Sign out**, or remove "Journal
  Notes" at
  [myaccount.google.com/permissions](https://myaccount.google.com/permissions).
  Your backups stay in your Drive until you delete the "Notes" folder
  there.
- **From your computer:** delete `~/.local/share/journal-notes/` (Notes
  and its backups), `~/Documents/Notes/` (exports and rescue copies),
  `~/.local/share/journal/` (the Journal) and `~/.config/journal-notes/`,
  and remove "Journal Notes: Google Drive" (and the Journal's own item,
  if you set a PIN) from your login keyring, in the Passwords and Keys
  app.

## Changes and questions

Changes to this policy are made in the open, in this file's history:
[docs/privacy.md](https://github.com/biswas2200/my-journal/commits/main/docs/privacy.md).
Questions: open an issue at
[github.com/biswas2200/my-journal/issues](https://github.com/biswas2200/my-journal/issues).
