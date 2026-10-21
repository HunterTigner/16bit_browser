"use strict";

(async () => {
  const canvas = document.getElementById("screen");
  const context = canvas.getContext("2d", { alpha: false });
  const filesKey = "pixel16.files.v1";
  const ramKey = "pixel16.ram";
  const inputModeKey = "pixel16.inputMode";
  const disksKey = "pixel16.disks.v1";
  const decoder = new TextDecoder();
  const encoder = new TextEncoder();
  let kernel;

  function bytesAt(pointer, length) {
    return new Uint8Array(kernel.memory.buffer, pointer, length);
  }

  function reportStorageError() {
    kernel.pixel16_storage_error();
  }

  function restoreDisk() {
    const saved = localStorage.getItem(filesKey);
    if (saved === null) return;
    const records = JSON.parse(saved);
    if (!Array.isArray(records)) {
      throw new Error("The saved C: drive data is invalid.");
    }
    for (const file of records) {
      if (!file || typeof file.name !== "string" || typeof file.ext !== "string" ||
          typeof file.content !== "string") {
        throw new Error("The saved C: drive file data is invalid.");
      }
      const name = encoder.encode(file.name);
      const ext = encoder.encode(file.ext);
      const content = encoder.encode(file.content);
      const scratch = kernel.pixel16_scratch_ptr();
      bytesAt(scratch, name.length).set(name);
      bytesAt(scratch + name.length, ext.length).set(ext);
      bytesAt(scratch + name.length + ext.length, content.length).set(content);
      const slot = kernel.pixel16_file_import(name.length, ext.length, content.length,
        Math.floor((Number(file.updated) || Date.now()) / 1000));
      if (slot < 0) throw new Error(`The WebAssembly disk rejected ${file.name}.${file.ext} (error ${slot}).`);
    }
  }

  function restoreVirtualDisks() {
    const saved = localStorage.getItem(disksKey);
    if (saved === null) return;
    const records = JSON.parse(saved);
    if (!Array.isArray(records)) throw new Error("The saved virtual disc list is invalid.");
    kernel.pixel16_disk_reset();
    for (const disk of records) {
      if (!disk || !Number.isInteger(disk.slot) || !Number.isInteger(disk.size_mb) ||
          kernel.pixel16_disk_restore(disk.slot, disk.size_mb) < 0) {
        throw new Error("The saved virtual disc configuration is invalid.");
      }
    }
  }

  function saveDisk() {
    const records = [];
    for (let index = 0; index < kernel.pixel16_file_count(); index++) {
      const slot = kernel.pixel16_file_slot_at(index);
      const name = decoder.decode(bytesAt(kernel.pixel16_file_name_ptr(slot), kernel.pixel16_file_name_length(slot)));
      const ext = decoder.decode(bytesAt(kernel.pixel16_file_extension_ptr(slot), kernel.pixel16_file_extension_length(slot)));
      const content = decoder.decode(bytesAt(kernel.pixel16_file_data_ptr(slot), kernel.pixel16_file_size(slot)));
      records.push({ name, ext, content, updated: kernel.pixel16_file_modified(slot) * 1000 });
    }
    try {
      localStorage.setItem(filesKey, JSON.stringify(records));
    } catch (error) {
      console.error("Pixel16 could not save the virtual C: drive.", error);
      reportStorageError();
    }
  }

  function saveVirtualDisks() {
    const records = [];
    for (let slot = 1; slot < 12; slot++) {
      const size_mb = kernel.pixel16_disk_size_mb(slot);
      if (size_mb) records.push({ slot, size_mb });
    }
    try {
      localStorage.setItem(disksKey, JSON.stringify(records));
    } catch (error) {
      console.error("Pixel16 could not save virtual disc settings.", error);
      kernel.pixel16_disk_storage_error();
    }
  }

  function openWebsiteFromGesture() {
    if (!kernel.pixel16_take_open_url()) return;
    const address = decoder.decode(bytesAt(kernel.pixel16_url_ptr(), kernel.pixel16_url_length()));
    try {
      const url = new URL(/^https?:\/\//i.test(address) ? address : `https://${address}`);
      if (url.protocol !== "https:" && url.protocol !== "http:") throw new Error("Only web addresses are supported.");
      window.open(url.href, "_blank", "noopener,noreferrer");
    } catch (error) {
      console.error("Pixel16 rejected the website address.", error);
      kernel.pixel16_browser_error();
    }
  }

  function isPublicDownloadUrl(url) {
    const hostname = url.hostname.toLowerCase().replace(/^\[|\]$/g, "");
    if (url.protocol !== "https:" && url.protocol !== "http:") return false;
    if (url.username || url.password || hostname === "localhost" ||
        hostname.endsWith(".localhost") || hostname.endsWith(".local") ||
        hostname.endsWith(".internal")) return false;
    if (hostname.includes(":")) return false;
    const octets = hostname.split(".");
    if (octets.length === 4 && octets.every(part => /^\d{1,3}$/.test(part) && Number(part) <= 255)) {
      const [first, second] = octets.map(Number);
      if (first === 0 || first === 10 || first === 127 ||
          (first === 169 && second === 254) ||
          (first === 172 && second >= 16 && second <= 31) ||
          (first === 192 && second === 168) || first >= 224) return false;
    }
    return true;
  }

  async function fetchRequestedFile() {
    const urlText = decoder.decode(bytesAt(kernel.pixel16_fetch_url_ptr(), kernel.pixel16_fetch_url_length()));
    try {
      const url = new URL(urlText);
      if (!isPublicDownloadUrl(url)) {
        kernel.pixel16_fetch_complete(5, 0);
        return;
      }
      const response = await fetch(url.href, {
        method: "GET",
        credentials: "omit",
        redirect: "error",
        cache: "no-store",
        mode: "cors"
      });
      if (!response.ok) {
        kernel.pixel16_fetch_complete(4, 0);
        return;
      }
      const declaredLength = Number(response.headers.get("content-length"));
      if (Number.isFinite(declaredLength) && declaredLength > 8192) {
        if (response.body) await response.body.cancel();
        kernel.pixel16_fetch_complete(2, 0);
        return;
      }
      if (!response.body) {
        kernel.pixel16_fetch_complete(1, 0);
        return;
      }
      const reader = response.body.getReader();
      const chunks = [];
      let length = 0;
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        length += value.length;
        if (length > 8192) {
          await reader.cancel();
          kernel.pixel16_fetch_complete(2, 0);
          return;
        }
        chunks.push(value);
      }
      const bytes = new Uint8Array(length);
      let offset = 0;
      for (const chunk of chunks) {
        bytes.set(chunk, offset);
        offset += chunk.length;
      }
      try {
        new TextDecoder("utf-8", { fatal: true }).decode(bytes);
      } catch {
        kernel.pixel16_fetch_complete(3, 0);
        return;
      }
      bytesAt(kernel.pixel16_fetch_scratch_ptr(), bytes.length).set(bytes);
      kernel.pixel16_fetch_complete(0, bytes.length);
      if (kernel.pixel16_take_dirty()) saveDisk();
    } catch (error) {
      console.error("Pixel16 could not fetch the requested file.", error);
      kernel.pixel16_fetch_complete(1, 0);
    }
  }

  function syncAfterInput() {
    if (kernel.pixel16_take_dirty()) saveDisk();
    if (kernel.pixel16_take_disk_dirty()) saveVirtualDisks();
    openWebsiteFromGesture();
    if (kernel.pixel16_take_fetch_request()) void fetchRequestedFile();
  }

  try {
    const response = await fetch("./pixel16.wasm");
    if (!response.ok) throw new Error(`Could not load pixel16.wasm (${response.status}).`);
    const result = await WebAssembly.instantiate(await response.arrayBuffer());
    kernel = result.instance.exports;
    if (kernel.pixel16_abi_version() !== 2) throw new Error("Unsupported Pixel16 kernel version.");
    kernel.pixel16_set_time(Math.floor(Date.now() / 1000));
    kernel.pixel16_set_clock(new Date().getHours() * 60 + new Date().getMinutes());
    kernel.pixel16_boot();
    try {
      const selected = Number(localStorage.getItem(ramKey));
      if (Number.isInteger(selected) && selected >= 0 && selected < 5) kernel.pixel16_set_ram_choice(selected);
      const inputMode = Number(localStorage.getItem(inputModeKey));
      if (Number.isInteger(inputMode) && inputMode >= 0 && inputMode < 2)
        kernel.pixel16_set_input_mode(inputMode);
      restoreDisk();
    } catch (error) {
      console.error("Pixel16 could not restore its virtual C: drive.", error);
      kernel.pixel16_file_clear();
      reportStorageError();
    }
    try {
      restoreVirtualDisks();
    } catch (error) {
      console.error("Pixel16 could not restore virtual disc settings.", error);
      kernel.pixel16_disk_reset();
      kernel.pixel16_disk_storage_error();
    }
  } catch (error) {
    console.error("Pixel16 could not start its WebAssembly desktop.", error);
    document.title = "Pixel16 - WebAssembly could not start";
    return;
  }

  canvas.addEventListener("pointerdown", event => {
    canvas.focus();
    const bounds = canvas.getBoundingClientRect();
    const x = Math.max(0, Math.min(319, Math.floor((event.clientX - bounds.left) * 320 / bounds.width)));
    const y = Math.max(0, Math.min(199, Math.floor((event.clientY - bounds.top) * 200 / bounds.height)));
    kernel.pixel16_set_time(Math.floor(Date.now() / 1000));
    kernel.pixel16_mouse(x, y);
    syncAfterInput();
    try {
      localStorage.setItem(ramKey, String(kernel.pixel16_ram_choice()));
      localStorage.setItem(inputModeKey, String(kernel.pixel16_input_mode()));
    } catch (error) { console.error("Pixel16 could not save its preferences.", error); }
  });

  const keyCodes = {
    Backspace: 8, Enter: 13, Escape: 27, ArrowLeft: 1000, ArrowRight: 1001,
    ArrowUp: 1002, ArrowDown: 1003, Delete: 1004, Tab: 1005
  };
  canvas.addEventListener("keydown", event => {
    if (event.ctrlKey || event.metaKey || event.altKey) return;
    const special = keyCodes[event.key] || 0;
    const codepoint = !special && event.key.length === 1 ? event.key.codePointAt(0) : 0;
    if (special || codepoint >= 32) event.preventDefault();
    kernel.pixel16_set_time(Math.floor(Date.now() / 1000));
    kernel.pixel16_key(special, codepoint);
    syncAfterInput();
    try {
      localStorage.setItem(ramKey, String(kernel.pixel16_ram_choice()));
      localStorage.setItem(inputModeKey, String(kernel.pixel16_input_mode()));
    } catch (error) { console.error("Pixel16 could not save its preferences.", error); }
  });

  document.addEventListener("paste", event => {
    const pasted = event.clipboardData && event.clipboardData.getData("text");
    if (!pasted) return;
    event.preventDefault();
    const bytes = encoder.encode(pasted);
    if (bytes.length <= kernel.pixel16_scratch_capacity()) {
      bytesAt(kernel.pixel16_scratch_ptr(), bytes.length).set(bytes);
    }
    kernel.pixel16_paste(bytes.length);
    syncAfterInput();
  });

  const image = context.createImageData(320, 200);
  function paint() {
    image.data.set(bytesAt(kernel.pixel16_framebuffer_ptr(), image.data.length));
    context.putImageData(image, 0, 0);
    requestAnimationFrame(paint);
  }
  requestAnimationFrame(paint);
  setInterval(() => {
    kernel.pixel16_set_clock(new Date().getHours() * 60 + new Date().getMinutes());
  }, 15000);
})();
