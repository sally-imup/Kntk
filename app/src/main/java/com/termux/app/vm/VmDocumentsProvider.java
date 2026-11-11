package com.termux.app.vm;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import android.webkit.MimeTypeMap;

import com.termux.shared.termux.TermuxConstants;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.UUID;

/**
 * Exposes the proot-distro Ubuntu rootfs through the Storage Access Framework, so it shows up in
 * the Files app and in file pickers.
 *
 * Document IDs are paths relative to the rootfs ("" or "/" is the root). Any ".." segment is
 * rejected so a crafted ID cannot escape the rootfs.
 */
public class VmDocumentsProvider extends DocumentsProvider {

    private static final String ROOT_ID = "ubuntu-vm";
    private static final String DISTRO = "ubuntu";

    private static final String[] DEFAULT_ROOT_PROJECTION = {
        Root.COLUMN_ROOT_ID, Root.COLUMN_FLAGS, Root.COLUMN_TITLE, Root.COLUMN_DOCUMENT_ID, Root.COLUMN_MIME_TYPES
    };
    private static final String[] DEFAULT_DOCUMENT_PROJECTION = {
        Document.COLUMN_DOCUMENT_ID, Document.COLUMN_MIME_TYPE, Document.COLUMN_DISPLAY_NAME,
        Document.COLUMN_LAST_MODIFIED, Document.COLUMN_FLAGS, Document.COLUMN_SIZE
    };

    private File getRootfs() {
        return new File(TermuxConstants.TERMUX_PREFIX_DIR_PATH + "/var/lib/proot-distro/installed-rootfs/" + DISTRO);
    }

    private File getFile(String documentId) throws FileNotFoundException {
        String rel = documentId == null ? "" : documentId;
        while (rel.startsWith("/")) rel = rel.substring(1);
        for (String segment : rel.split("/")) {
            if (segment.equals("..")) throw new FileNotFoundException("Invalid document id");
        }
        File file = rel.isEmpty() ? getRootfs() : new File(getRootfs(), rel);
        if (!file.exists()) throw new FileNotFoundException("Missing " + documentId);
        return file;
    }

    private static String documentIdFor(File rootfs, File file) {
        String rel = file.getAbsolutePath().substring(rootfs.getAbsolutePath().length());
        return rel.isEmpty() ? "/" : rel;
    }

    private static String mimeTypeOf(File file) {
        if (file.isDirectory()) return Document.MIME_TYPE_DIR;
        String name = file.getName();
        int dot = name.lastIndexOf('.');
        if (dot >= 0) {
            String ext = name.substring(dot + 1).toLowerCase();
            String mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
            if (mime != null) return mime;
        }
        return "application/octet-stream";
    }

    private static int flagsFor(File file) {
        int flags = Document.FLAG_SUPPORTS_DELETE | Document.FLAG_SUPPORTS_RENAME;
        if (file.isDirectory()) flags |= Document.FLAG_DIR_SUPPORTS_CREATE;
        else flags |= Document.FLAG_SUPPORTS_WRITE;
        return flags;
    }

    /** Builds one row in projection order (addRow works on minSdk 24, unlike newRow). */
    private void includeFile(MatrixCursor cursor, String[] projection, File rootfs, File file) {
        Object[] row = new Object[projection.length];
        for (int i = 0; i < projection.length; i++) {
            switch (projection[i]) {
                case Document.COLUMN_DOCUMENT_ID: row[i] = documentIdFor(rootfs, file); break;
                case Document.COLUMN_DISPLAY_NAME: row[i] = file.equals(rootfs) ? "Ubuntu VM" : file.getName(); break;
                case Document.COLUMN_SIZE: row[i] = file.isDirectory() ? null : file.length(); break;
                case Document.COLUMN_MIME_TYPE: row[i] = mimeTypeOf(file); break;
                case Document.COLUMN_LAST_MODIFIED: row[i] = file.lastModified(); break;
                case Document.COLUMN_FLAGS: row[i] = flagsFor(file); break;
                default: row[i] = null;
            }
        }
        cursor.addRow(row);
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public Cursor queryRoots(String[] projection) {
        String[] columns = resolveRootProjection(projection);
        MatrixCursor result = new MatrixCursor(columns);
        Object[] row = new Object[columns.length];
        for (int i = 0; i < columns.length; i++) {
            switch (columns[i]) {
                case Root.COLUMN_ROOT_ID: row[i] = ROOT_ID; break;
                case Root.COLUMN_DOCUMENT_ID: row[i] = "/"; break;
                case Root.COLUMN_TITLE: row[i] = "Ubuntu VM"; break;
                case Root.COLUMN_FLAGS: row[i] = Root.FLAG_LOCAL_ONLY | Root.FLAG_SUPPORTS_CREATE | Root.FLAG_SUPPORTS_IS_CHILD; break;
                case Root.COLUMN_MIME_TYPES: row[i] = "*/*"; break;
                default: row[i] = null;
            }
        }
        result.addRow(row);
        return result;
    }

    @Override
    public Cursor queryDocument(String documentId, String[] projection) throws FileNotFoundException {
        String[] columns = resolveDocumentProjection(projection);
        MatrixCursor result = new MatrixCursor(columns);
        includeFile(result, columns, getRootfs(), getFile(documentId));
        return result;
    }

    @Override
    public Cursor queryChildDocuments(String parentDocumentId, String[] projection, String sortOrder)
            throws FileNotFoundException {
        String[] columns = resolveDocumentProjection(projection);
        MatrixCursor result = new MatrixCursor(columns);
        File parent = getFile(parentDocumentId);
        File[] children = parent.listFiles();
        if (children != null) {
            for (File child : children) includeFile(result, columns, getRootfs(), child);
        }
        return result;
    }

    @Override
    public ParcelFileDescriptor openDocument(String documentId, String mode, CancellationSignal signal)
            throws FileNotFoundException {
        File file = getFile(documentId);
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.parseMode(mode));
    }

    @Override
    public String createDocument(String parentDocumentId, String mimeType, String displayName)
            throws FileNotFoundException {
        File parent = getFile(parentDocumentId);
        if (!parent.isDirectory()) throw new FileNotFoundException(parentDocumentId + " is not a folder");
        File created = new File(parent, displayName);
        try {
            boolean ok = Document.MIME_TYPE_DIR.equals(mimeType) ? created.mkdir() : created.createNewFile();
            if (!ok) throw new FileNotFoundException("Could not create " + displayName);
        } catch (IOException e) {
            throw new FileNotFoundException(e.getMessage());
        }
        return documentIdFor(getRootfs(), created);
    }

    @Override
    public void deleteDocument(String documentId) throws FileNotFoundException {
        File file = getFile(documentId);
        if (file.equals(getRootfs())) throw new FileNotFoundException("Refusing to delete the rootfs");
        if (!deleteRecursively(file)) throw new FileNotFoundException("Could not delete " + documentId);
    }

    @Override
    public String renameDocument(String documentId, String displayName) throws FileNotFoundException {
        File file = getFile(documentId);
        File target = new File(file.getParentFile(), displayName);
        if (target.exists() || !file.renameTo(target)) throw new FileNotFoundException("Could not rename");
        return documentIdFor(getRootfs(), target);
    }

    @Override
    public boolean isChildDocument(String parentDocumentId, String documentId) {
        try {
            String parent = getFile(parentDocumentId).getCanonicalPath();
            String child = getFile(documentId).getCanonicalPath();
            return child.startsWith(parent + File.separator);
        } catch (IOException e) { // FileNotFoundException is an IOException
            return false;
        }
    }

    private static boolean deleteRecursively(File file) {
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) for (File c : children) if (!deleteRecursively(c)) return false;
        }
        return file.delete();
    }

    private static String[] resolveRootProjection(String[] projection) {
        return projection != null ? projection : DEFAULT_ROOT_PROJECTION;
    }

    private static String[] resolveDocumentProjection(String[] projection) {
        return projection != null ? projection : DEFAULT_DOCUMENT_PROJECTION;
    }
}
