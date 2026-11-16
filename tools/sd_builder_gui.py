#!/usr/bin/env python3
"""Six Guns NX SD Builder: a window around build_sd.py (same checks, same output).

    python3 tools/sd_builder_gui.py        (or the packaged "Six Guns SD Builder" app)

Pick the official Six Guns 2.9.9a APK and the folder with its two OBB files, choose where to write,
press Build. Only official, untouched game files are accepted (see build_sd.py)."""
import os
import queue
import subprocess
import sys
import threading

os.environ.setdefault("TK_SILENCE_DEPRECATION", "1")  # macOS system Tk 8.5 prints a warning otherwise
import tkinter as tk  # noqa: E402
from tkinter import filedialog, messagebox, ttk  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_sd  # noqa: E402

TITLE = "Six Guns NX SD Builder"


class App:
    def __init__(self, root):
        self.root = root
        root.title(TITLE)
        root.minsize(640, 520)
        self.q = queue.Queue()
        self.busy = False
        self.apk = tk.StringVar()
        self.obb_dir = tk.StringVar()
        self.obb_files = []
        self.out = tk.StringVar(value=os.path.join(os.path.expanduser("~"), "Desktop", "SixGuns SD"))
        self.nro = tk.StringVar(value=build_sd.default_nro() or "")
        self.stadium = tk.BooleanVar(value=build_sd.have_numpy())
        self.lan = tk.BooleanVar(value=True)

        f = ttk.Frame(root, padding=12)
        f.pack(fill="both", expand=True)
        f.columnconfigure(1, weight=1)
        ttk.Label(f, text="Build the Switch SD card from your own, official Six Guns 2.9.9a files.",
                  font=("TkDefaultFont", 12, "bold")).grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 8))

        def row(r, label, var, cmd, hint):
            ttk.Label(f, text=label).grid(row=r, column=0, sticky="w", pady=3)
            ttk.Entry(f, textvariable=var).grid(row=r, column=1, sticky="ew", padx=6)
            ttk.Button(f, text="Choose…", command=cmd).grid(row=r, column=2)
            ttk.Label(f, text=hint, foreground="#666").grid(row=r + 1, column=1, columnspan=2, sticky="w")

        row(1, "Game APK", self.apk, self.pick_apk, "Six Guns 2.9.9a APK signed by Gameloft (or an XAPK holding it)")
        row(3, "OBB folder", self.obb_dir, self.pick_obb_dir,
            "Folder with main.29431… and patch.29523… (phone: Android/obb/com.gameloft.android.ANMP.GloftSXHM)")
        row(5, "Output folder", self.out, self.pick_out, "Created for you; the SD card files go in <this>/switch/sixguns")
        if not self.nro.get():
            # the packaged app and a source checkout with a built port carry sixguns.nro; only otherwise ask for it
            row(7, "sixguns.nro", self.nro, self.pick_nro, "The port itself: build it (port/build.sh) or take it from the releases page")

        opts = ttk.Frame(f)
        opts.grid(row=9, column=0, columnspan=3, sticky="w", pady=(8, 4))
        self.st = ttk.Checkbutton(opts, text="Add the Stadium demo map (custom multiplayer map)", variable=self.stadium)
        self.st.pack(anchor="w")
        if not build_sd.have_numpy():
            # only when run from source: the packaged app carries numpy
            self.st.state(["disabled"])
            self.np_row = ttk.Frame(opts)
            self.np_row.pack(anchor="w")
            ttk.Label(self.np_row, text="   The Stadium map needs the numpy package.", foreground="#666").pack(side="left")
            self.b_numpy = ttk.Button(self.np_row, text="Install numpy…", command=self.install_numpy)
            self.b_numpy.pack(side="left", padx=6)
        ttk.Checkbutton(opts, text="Turn on LAN multiplayer", variable=self.lan).pack(anchor="w")

        btns = ttk.Frame(f)
        btns.grid(row=10, column=0, columnspan=3, sticky="ew", pady=6)
        self.b_check = ttk.Button(btns, text="Check files", command=lambda: self.start(True))
        self.b_build = ttk.Button(btns, text="Build SD card", command=lambda: self.start(False))
        self.b_open = ttk.Button(btns, text="Open output folder", command=self.open_out)
        self.b_help = ttk.Button(btns, text="How to install on Switch", command=self.show_install)
        self.b_check.pack(side="left")
        self.b_build.pack(side="left", padx=6)
        self.b_open.pack(side="right")
        self.b_help.pack(side="right", padx=6)
        self.bar = ttk.Progressbar(f, mode="indeterminate")
        self.bar.grid(row=11, column=0, columnspan=3, sticky="ew")

        self.log = tk.Text(f, height=14, wrap="word", state="disabled")
        self.log.grid(row=12, column=0, columnspan=3, sticky="nsew", pady=(6, 0))
        f.rowconfigure(12, weight=1)
        self.status = ttk.Label(f, text="Choose your game files, then Check files or Build SD card.")
        self.status.grid(row=13, column=0, columnspan=3, sticky="w", pady=(6, 0))
        root.after(100, self.poll)

    def show_install(self):
        """The Switch install steps in a scrollable window."""
        w = tk.Toplevel(self.root)
        w.title("Install on your Switch")
        body = ttk.Frame(w)
        body.pack(fill="both", expand=True)
        t = tk.Text(body, wrap="word", width=108, height=34, padx=12, pady=10)
        sb = ttk.Scrollbar(body, command=t.yview)
        t.configure(yscrollcommand=sb.set)
        t.insert("1.0", build_sd.INSTALL_STEPS.replace("<OUT>", os.path.join(self.out.get(), "switch", "sixguns")))
        t.configure(state="disabled")
        sb.pack(side="right", fill="y")
        t.pack(side="left", fill="both", expand=True)
        ttk.Button(w, text="Close", command=w.destroy).pack(pady=6)

    # ---- pickers
    def pick_apk(self):
        p = filedialog.askopenfilename(title="Six Guns APK", filetypes=[("Android app", "*.apk *.xapk *.apks"), ("All", "*")])
        if p:
            self.apk.set(p)
            if not self.obb_dir.get():  # an OBB folder often sits next to the APK
                guess = os.path.join(os.path.dirname(p), build_sd.PKG)
                if os.path.isdir(guess):
                    self.obb_dir.set(guess)

    def pick_obb_dir(self):
        p = filedialog.askdirectory(title="Folder with the two OBB files")
        if p:
            self.obb_dir.set(p)

    def pick_out(self):
        p = filedialog.askdirectory(title="Where to write the SD card files")
        if p:
            self.out.set(p)

    def pick_nro(self):
        p = filedialog.askopenfilename(title="sixguns.nro", filetypes=[("Switch homebrew", "*.nro"), ("All", "*")])
        if p:
            self.nro.set(p)

    def open_out(self):
        p = os.path.join(self.out.get(), "switch", "sixguns")
        p = p if os.path.isdir(p) else self.out.get()
        if not os.path.isdir(p):
            messagebox.showinfo(TITLE, "Nothing built yet.")
            return
        if sys.platform == "win32":
            os.startfile(p)  # noqa: S606 - opening a folder the user chose
        else:
            subprocess.run(["open" if sys.platform == "darwin" else "xdg-open", p])

    def install_numpy(self):
        """Offers to install numpy for this Python (pip; --user unless in a virtual environment)."""
        if self.busy:
            return
        user = sys.prefix == sys.base_prefix
        cmd = [sys.executable, "-m", "pip", "install"] + (["--user"] if user else []) + ["numpy"]
        if not messagebox.askyesno(TITLE, "Install numpy now?\n\nThis downloads it from the Python Package Index (pypi.org) "
                                   "and runs:\n" + " ".join(cmd)):
            return
        self.busy = True
        self.b_numpy.state(["disabled"])
        self.bar.start(12)
        self.status.configure(text="Installing numpy…")

        def work():
            try:
                r = subprocess.run(cmd, capture_output=True, text=True)
                if r.returncode == 0:
                    import importlib
                    import site
                    if user and site.getusersitepackages() not in sys.path:
                        sys.path.append(site.getusersitepackages())
                    importlib.invalidate_caches()
                ok = r.returncode == 0 and build_sd.have_numpy()
                self.q.put(("numpy", "numpy installed: the Stadium map is available." if ok else
                            "numpy could not be installed:\n" + (r.stderr or r.stdout)[-800:]))
            except Exception as e:
                self.q.put(("numpy", f"numpy could not be installed: {e!r}"))
        threading.Thread(target=work, daemon=True).start()

    # ---- work
    def say(self, msg):
        self.q.put(("log", msg))

    def start(self, check_only):
        if self.busy:
            return
        if not self.apk.get():
            messagebox.showwarning(TITLE, "Choose the Six Guns APK first.")
            return
        self.busy = True
        for b in (self.b_check, self.b_build):
            b.state(["disabled"])
        self.bar.start(12)
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")
        self.status.configure(text="Checking…" if check_only else "Building… (about a minute)")
        args = dict(apk=self.apk.get(), obb_dir=self.obb_dir.get() or None, out=self.out.get(), nro=self.nro.get() or None,
                    stadium=self.stadium.get(), lan=self.lan.get(), check_only=check_only, log=self.say)
        threading.Thread(target=self.run, args=(args,), daemon=True).start()

    def run(self, args):
        try:
            warnings = build_sd.build(**args)
            self.q.put(("done", ("Files are official." if args["check_only"] else "SD card files ready.") +
                        (f" {len(warnings)} warning(s)." if warnings else "")))
        except build_sd.Refused as e:
            self.q.put(("refused", str(e)))
        except Exception as e:  # unexpected: show it instead of dying silently
            self.q.put(("refused", f"Unexpected error: {e!r}"))

    def poll(self):
        try:
            while True:
                kind, msg = self.q.get_nowait()
                if kind == "log":
                    self.log.configure(state="normal")
                    self.log.insert("end", msg + "\n")
                    self.log.see("end")
                    self.log.configure(state="disabled")
                    continue
                self.busy = False
                for b in (self.b_check, self.b_build):
                    b.state(["!disabled"])
                self.bar.stop()
                if kind == "numpy":
                    ok = build_sd.have_numpy()
                    if ok:
                        self.np_row.destroy()
                        self.st.state(["!disabled"])
                        self.stadium.set(True)
                    else:
                        self.b_numpy.state(["!disabled"])
                    self.status.configure(text=msg.splitlines()[0])
                    (messagebox.showinfo if ok else messagebox.showerror)(TITLE, msg)
                    continue
                self.status.configure(text=msg if kind == "done" else "Refused: see the message.")
                if kind == "done" and msg.startswith("SD card files ready"):
                    self.show_install()
                if kind == "refused":
                    self.say("REFUSED: " + msg)
                    messagebox.showerror(TITLE, msg)
        except queue.Empty:
            pass
        self.root.after(100, self.poll)


def main():
    # Self-test without a window (packaging checks / CI): SIXGUNS_SD_SELFTEST='{"apk": ..., "obb_dir": ..., "out": ...}'
    st = os.environ.get("SIXGUNS_SD_SELFTEST")
    if st is not None:
        import json
        args = json.loads(st) if st.strip() else {}
        print("selftest: frozen=%s base=%s nro=%s numpy=%s" % (build_sd.FROZEN, build_sd.BASE, build_sd.default_nro(),
                                                               build_sd.have_numpy()), flush=True)
        if args:
            try:
                build_sd.build(log=lambda m: print(m, flush=True), **args)
            except build_sd.Refused as e:
                print("REFUSED: " + str(e), flush=True)
                sys.exit(2)
        sys.exit(0)
    if sys.platform == "darwin" and tk.TkVersion < 8.6:
        print("This Python has Tk %s, whose windows do not draw on current macOS. Use the packaged app, or a Python "
              "from python.org (Tk 8.6+). The command-line builder works: python3 tools/build_sd.py --help" % tk.TkVersion)
        sys.exit(1)
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
