package dev.relift.app;

import android.app.Activity;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.window.OnBackInvokedDispatcher;

/**
 * Game settings, opened from the app's Game settings shortcut (a long press
 * on the game's icon) while the game is not running: the same pages, without
 * the restart banner; a choice applies at the next start. Import again opens
 * the import (DataActivity) at once, with no game to close.
 */
public class SettingsActivity extends Activity implements Ui.Host {
    private Ui ui;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        ui = new Ui(this, new Drivers(this), this, true);
        setContentView(ui.root);
        if (Build.VERSION.SDK_INT >= 33) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT,
                    () -> ui.back());
        }
        ui.open("gpu_driver".equals(getIntent().getStringExtra("relift_open")) ? Ui.Page.DRIVER : Ui.Page.HOME);
    }

    @Override
    public void onBackPressed() {
        ui.back();
    }

    @Override
    protected void onActivityResult(int request, int code, Intent data) {
        super.onActivityResult(request, code, data);
        if (request == Ui.REQ_PICK) {
            ui.picked(code, data);
        }
    }

    @Override
    public void closed(int next) {
        finish();
    }

    @Override
    public void restart() {
    }

    @Override
    public String loaded() {
        return null;
    }

    @Override
    public void openData(String action) {
        if (DataActivity.ACTION_DELETE.equals(action)) {
            DataPages.delete(this, ui::render);
            return;
        }
        DataActivity.open(this, action);
    }
}
